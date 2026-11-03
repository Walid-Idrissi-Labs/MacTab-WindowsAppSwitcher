#include "pch.h"
#include <shobjidl.h>

#include "desktops.h"
#include "com.h"
#include "common.h"
#include "config.h"
#include "diag.h"
#include "hotkey.h"

namespace mactab::desktops {
namespace {

constexpr wchar_t kRoot[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";

// Win10 keeps the desktop being viewed per logon session, under SessionInfo.
// Win11 moved it next to the list. PowerToys branches on exactly this, and both
// are read here rather than sniffing the build, because a build check would be
// one more thing to be wrong about on a machine nobody here can see.
std::wstring SessionKeyPath() {
    DWORD session = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);

    wchar_t path[160];
    ::wsprintfW(path,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\"
                L"SessionInfo\\%lu\\VirtualDesktops",
                session);
    return path;
}

bool ReadBinary(const std::wstring& key, const wchar_t* value,
                std::vector<BYTE>& out) {
    DWORD type = 0, size = 0;
    if (::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_BINARY,
                       &type, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return false;

    out.resize(size);
    return ::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_BINARY,
                          nullptr, out.data(), &size) == ERROR_SUCCESS;
}

// The desktop being viewed, from the registry alone.
//
// Deliberately without Query's fallback to our own window's desktop. That
// fallback answers "which desktop is this WINDOW on", which is the right answer
// at rest and the wrong one entirely while waiting for a switch: a window does
// not move when the view does, so a wait built on it would return instantly and
// always.
bool CurrentFromRegistry(GUID& out) {
    std::vector<BYTE> raw;
    if ((!ReadBinary(kRoot, L"CurrentVirtualDesktop", raw) &&
         !ReadBinary(SessionKeyPath(), L"CurrentVirtualDesktop", raw)) ||
        raw.size() < sizeof(GUID))
        return false;

    std::memcpy(&out, raw.data(), sizeof(GUID));
    return true;
}

std::wstring GuidToBraces(const GUID& id) {
    wchar_t text[64] = L"";
    ::StringFromGUID2(id, text, ARRAYSIZE(text));
    return text;
}

// The shell's own name for a desktop, if the user has ever renamed it.
//
// Renaming only shipped in Windows 10 2004, and the value only exists once a
// desktop has actually been renamed, so on most machines this returns nothing
// for every desktop and the ordinal name is what gets shown. That is also what
// Windows itself shows.
std::wstring NameOf(const GUID& id, int index) {
    const std::wstring key = std::wstring(kRoot) + L"\\Desktops\\" + GuidToBraces(id);

    wchar_t buffer[256] = L"";
    DWORD   size        = sizeof(buffer);
    if (::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"Name", RRF_RT_REG_SZ,
                       nullptr, buffer, &size) == ERROR_SUCCESS && buffer[0])
        return buffer;

    wchar_t fallback[32];
    ::wsprintfW(fallback, L"Desktop %d", index + 1);
    return fallback;
}

// One IVirtualDesktopManager for the process.
//
// UI thread only. It is a COM object with apartment affinity and every caller
// here is on the thread that owns the overlay.
IVirtualDesktopManager* Manager() {
    static ComPtr<IVirtualDesktopManager> manager;
    static bool tried = false;

    if (!tried) {
        tried = true;
        if (FAILED(::CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(manager.Put()))))
            MACTAB_WARN("desktops: IVirtualDesktopManager unavailable");
    }
    return manager.Get();
}

// --- The shell's own interface ----------------------------------------------
//
// See the header for why this exists and what it is allowed to touch. In short:
// three known IIDs, only the slots that have never moved, and a check against
// the registry before anything is trusted.

// Declared without uuids, because the IID depends on the build. The slot order
// is what matters, and it is the same for all three layouts below.
struct IShellDesktop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsViewVisible(IUnknown* view, BOOL* visible) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetId(GUID* id) = 0;
};

struct IShellDesktops : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(IUnknown* view, IShellDesktop* desktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(IUnknown* view, BOOL* can) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(IShellDesktop** desktop) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray** desktops) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(IShellDesktop* from, UINT direction,
                                                         IShellDesktop** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktop(IShellDesktop* desktop) = 0;
    // Nothing past slot 9, ever. Slot 10 moved inside one IID in 26100.2605.
};

// IApplicationViewCollection. Same IID and order from Windows 10 1809 onward.
struct IShellViews : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray** views) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray** views) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR id, IObjectArray** views) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND hwnd, IUnknown** view) = 0;
};

// Kept as text and parsed, so no GUID is ever transcribed into bytes by hand.
constexpr wchar_t kImmersiveShell[]   = L"{C2F03A33-21F5-47FA-B4BB-156362A2F239}";
constexpr wchar_t kDesktopsService[]  = L"{C5E0CDCA-7B6E-41B2-9FC4-D93975CC467B}";
constexpr wchar_t kViewsInterface[]   = L"{1841C6D7-4F9D-42C0-AF41-8747538F10E5}";

struct Layout {
    const char*    name;
    const wchar_t* desktops;   // IVirtualDesktopManagerInternal
    const wchar_t* desktop;    // IVirtualDesktop
    bool           win11;
};

// Newest first. Sources: Ciantic/VirtualDesktopAccessor and MScholtes/VirtualDesktop,
// which agree on all three.
constexpr Layout kLayouts[] = {
    { "Windows 11 23H2 (22631.3085) to 25H2",
      L"{53F5CA0B-158F-4124-900C-057158060B27}", L"{3F07F4BE-B107-441A-AF0F-39D82529072C}", true },
    { "Windows 11 22H2 (22621.2215) to 23H2 (22631.3007)",
      L"{A3175F2D-239C-4BD2-8AA0-EEBA8B0B138E}", L"{3F07F4BE-B107-441A-AF0F-39D82529072C}", true },
    { "Windows 10 1809 to 22H2",
      L"{F31574D6-B682-4CDC-BD56-1827860ABEC6}", L"{FF72FFDD-BE7E-43FC-9C03-AD81681E88E4}", false },
};

GUID ParseGuid(const wchar_t* text) {
    GUID id{};
    ::CLSIDFromString(text, &id);
    return id;
}

struct Shell {
    ComPtr<IShellDesktops> desktops;
    ComPtr<IShellViews>    views;
    GUID                   desktopIid{};
};

// UI thread only, like Manager().
Shell g_shell;

// Set when a layout answered QueryService but failed the check, or when no
// layout answered at all once the shell was up. Either way this build is not one
// the slots above were checked against, and asking again will not change that.
bool g_shellRefused = false;

// Every desktop the interface lists, in its order, and the one being viewed.
//
// Split from SelfTest so the __try there guards a function with no destructors
// of its own, which is what MSVC requires of one.
bool ReadShellDesktops(IShellDesktops* desktops, const GUID& desktopIid,
                       std::vector<GUID>& out, GUID& current, const char** why) {
    UINT count = 0;
    if (FAILED(desktops->GetCount(&count)) || count == 0) {
        *why = "GetCount";
        return false;
    }

    ComPtr<IObjectArray> array;
    UINT listed = 0;
    if (FAILED(desktops->GetDesktops(array.Put())) || !array ||
        FAILED(array->GetCount(&listed)) || listed != count) {
        *why = "GetDesktops";
        return false;
    }

    for (UINT i = 0; i < listed; ++i) {
        ComPtr<IShellDesktop> desktop;
        GUID id{};
        if (FAILED(array->GetAt(i, desktopIid, desktop.PutVoid())) || !desktop ||
            FAILED(desktop->GetId(&id))) {
            *why = "IVirtualDesktop::GetId";
            return false;
        }
        out.push_back(id);
    }

    ComPtr<IShellDesktop> viewed;
    if (FAILED(desktops->GetCurrentDesktop(viewed.Put())) || !viewed ||
        FAILED(viewed->GetId(&current))) {
        *why = "GetCurrentDesktop";
        return false;
    }
    return true;
}

#if defined(_MSC_VER)
bool ReadShellDesktopsGuarded(IShellDesktops* desktops, const GUID& desktopIid,
                              std::vector<GUID>* out, GUID* current, const char** why) {
    // A slot with a different signature faults in our proxy rather than
    // returning an error. Refusing the build is the right answer to that too.
    __try {
        return ReadShellDesktops(desktops, desktopIid, *out, *current, why);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a fault inside the call";
        return false;
    }
}
#else
bool ReadShellDesktopsGuarded(IShellDesktops* desktops, const GUID& desktopIid,
                              std::vector<GUID>* out, GUID* current, const char** why) {
    return ReadShellDesktops(desktops, desktopIid, *out, *current, why);
}
#endif

// Does this interface describe the desktops that are actually there?
//
// Compared with the registry, which the shell writes and which has nothing to
// do with any vtable. Same desktops, same order, and the viewed one among them.
// Without a registry list (one desktop, never added to) the interface still has
// to agree with itself across four different calls.
bool SelfTest(IShellDesktops* desktops, const GUID& desktopIid, const char** why) {
    std::vector<GUID> listed;
    GUID current{};
    if (!ReadShellDesktopsGuarded(desktops, desktopIid, &listed, &current, why))
        return false;

    if (std::none_of(listed.begin(), listed.end(),
                     [&](const GUID& id) { return ::IsEqualGUID(id, current) != FALSE; })) {
        *why = "the viewed desktop is not in its own list";
        return false;
    }

    std::vector<BYTE> raw;
    if (ReadBinary(kRoot, L"VirtualDesktopIDs", raw) && raw.size() >= sizeof(GUID)) {
        if (raw.size() / sizeof(GUID) != listed.size()) {
            *why = "a different number of desktops from the registry";
            return false;
        }
        for (size_t i = 0; i < listed.size(); ++i) {
            if (std::memcmp(raw.data() + i * sizeof(GUID), &listed[i], sizeof(GUID)) != 0) {
                *why = "different desktops from the registry";
                return false;
            }
        }
    }
    return true;
}

// The shell's desktop interface, connected and checked, or null.
IShellDesktops* ShellDesktops() {
    if (config::Current().desktopMoves == L"off") return nullptr;
    if (g_shellRefused) return nullptr;
    if (g_shell.desktops) return g_shell.desktops.Get();

    const GUID shellClass = ParseGuid(kImmersiveShell);
    ComPtr<IServiceProvider> shell;
    HRESULT hr = ::CoCreateInstance(shellClass, nullptr, CLSCTX_LOCAL_SERVER,
                                    IID_PPV_ARGS(shell.Put()));
    if (FAILED(hr)) {
        // Explorer not up yet, which is normal at logon. Asked again next time.
        MACTAB_DIAG("desktops: the shell is not answering (0x%08lX)",
                    static_cast<unsigned long>(hr));
        return nullptr;
    }

    const GUID viewsIid = ParseGuid(kViewsInterface);
    ComPtr<IShellViews> views;
    hr = shell->QueryService(viewsIid, viewsIid, views.PutVoid());
    if (FAILED(hr) || !views) {
        MACTAB_DIAG("desktops: no view collection from the shell (0x%08lX)",
                    static_cast<unsigned long>(hr));
        return nullptr;
    }

    const uint32_t build   = WindowsBuildNumber();
    const bool     win11   = build >= 22000;
    const GUID     service = ParseGuid(kDesktopsService);

    for (const Layout& layout : kLayouts) {
        if (layout.win11 != win11) continue;

        ComPtr<IShellDesktops> desktops;
        if (FAILED(shell->QueryService(service, ParseGuid(layout.desktops),
                                       desktops.PutVoid())) || !desktops)
            continue;

        const GUID desktopIid = ParseGuid(layout.desktop);
        const char* why = "";
        if (!SelfTest(desktops.Get(), desktopIid, &why)) {
            g_shellRefused = true;
            MACTAB_WARN("desktops: the shell's interface for %s failed its check on "
                        "build %u (%s); other applications' windows will not be moved",
                        layout.name, build, why);
            return nullptr;
        }

        g_shell.desktops   = std::move(desktops);
        g_shell.views      = std::move(views);
        g_shell.desktopIid = desktopIid;
        MACTAB_DIAG("desktops: moving windows through the shell, layout %s, build %u",
                    layout.name, build);
        return g_shell.desktops.Get();
    }

    g_shellRefused = true;
    MACTAB_WARN("desktops: build %u has a desktop interface MacTab has not been "
                "checked against; other applications' windows will not be moved",
                build);
    return nullptr;
}

// Explorer restarted, or is restarting, under us.
bool ShellWentAway(HRESULT hr) {
    return hr == RPC_E_DISCONNECTED || hr == RPC_E_SERVER_DIED ||
           hr == RPC_E_SERVER_DIED_DNE ||
           hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
           hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED);
}

// The shell's object for the desktop with this id. Walked rather than looked up,
// because FindDesktop is past slot 9.
HRESULT FindShellDesktop(IShellDesktops* desktops, const GUID& id,
                         ComPtr<IShellDesktop>& out) {
    ComPtr<IObjectArray> array;
    HRESULT hr = desktops->GetDesktops(array.Put());
    if (FAILED(hr)) return hr;
    if (!array) return E_FAIL;

    UINT count = 0;
    hr = array->GetCount(&count);
    if (FAILED(hr)) return hr;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IShellDesktop> desktop;
        GUID candidate{};
        hr = array->GetAt(i, g_shell.desktopIid, desktop.PutVoid());
        if (FAILED(hr)) return hr;
        if (!desktop || FAILED(desktop->GetId(&candidate))) continue;
        if (::IsEqualGUID(candidate, id)) {
            out = std::move(desktop);
            return S_OK;
        }
    }
    return TYPE_E_ELEMENTNOTFOUND;
}

// Run `call` against the shell, reconnecting once if explorer has restarted
// since the last call and the cached proxies point at a process that is gone.
template <typename F>
HRESULT CallShell(F&& call) {
    HRESULT hr = E_NOINTERFACE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        IShellDesktops* desktops = ShellDesktops();
        if (!desktops) return E_NOINTERFACE;

        hr = call(desktops);
        if (!ShellWentAway(hr)) return hr;

        MACTAB_WARN("desktops: the shell went away (0x%08lX); reconnecting",
                    static_cast<unsigned long>(hr));
        g_shell = Shell{};
    }
    return hr;
}

HRESULT MoveThroughShell(IShellDesktops* desktops, HWND hwnd, const GUID& target) {
    ComPtr<IUnknown> view;
    HRESULT hr = g_shell.views->GetViewForHwnd(hwnd, view.Put());
    if (FAILED(hr)) return hr;
    if (!view) return E_FAIL;

    // False for a window pinned to every desktop, among others.
    BOOL can = FALSE;
    hr = desktops->CanViewMoveDesktops(view.Get(), &can);
    if (FAILED(hr)) return hr;
    if (!can) return E_ACCESSDENIED;

    ComPtr<IShellDesktop> desktop;
    hr = FindShellDesktop(desktops, target, desktop);
    if (FAILED(hr)) return hr;

    return desktops->MoveViewToDesktop(view.Get(), desktop.Get());
}

void AppendKey(std::vector<INPUT>& inputs, WORD vk, bool up) {
    INPUT input{};
    input.type           = INPUT_KEYBOARD;
    input.ki.wVk         = vk;
    input.ki.wScan       = static_cast<WORD>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags     = up ? KEYEVENTF_KEYUP : 0u;
    input.ki.dwExtraInfo = hotkey::kInjectionTag;
    inputs.push_back(input);
}

// Send Ctrl+Win+<key> as if the user had pressed it.
//
// Two things have to happen before the chord itself. Any modifier the user is
// physically holding has to be released, or the shell sees a different chord
// than the one intended: with Win still down from the gesture that opened the
// overlay, Ctrl+Win+D arrives as something the shell ignores. And every event
// carries our injection tag so the keyboard hook passes it through instead of
// feeding it back into its own state machine.
//
// What deliberately does NOT happen is putting those modifiers back afterwards.
// It looks like an omission, because the user may well still be holding Win
// while the system now believes it is up, and their next chord will not be read
// as one until they release and press again.
//
// Restoring them is worse. A Win we press again is a Win the user then releases
// with nothing in between, which is the definition of a Win tap, and the Start
// menu opens on top of the overlay. Alt has the same shape and lands on the
// menu bar instead. Leaving them logically up costs a chord nobody is trying to
// type in the middle of Mission Control, and the stray key-up that arrives when
// they let go is ignored for want of a matching key-down.
bool SendChord(WORD key) {
    std::vector<INPUT> inputs;
    inputs.reserve(10);

    auto held = [](int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; };

    if (held(VK_LWIN))    AppendKey(inputs, VK_LWIN,    true);
    if (held(VK_RWIN))    AppendKey(inputs, VK_RWIN,    true);
    if (held(VK_SHIFT))   AppendKey(inputs, VK_SHIFT,   true);
    if (held(VK_MENU))    AppendKey(inputs, VK_MENU,    true);
    if (held(VK_CONTROL)) AppendKey(inputs, VK_CONTROL, true);

    AppendKey(inputs, VK_LCONTROL, false);
    AppendKey(inputs, VK_LWIN,     false);
    AppendKey(inputs, key,         false);
    AppendKey(inputs, key,         true);
    AppendKey(inputs, VK_LWIN,     true);
    AppendKey(inputs, VK_LCONTROL, true);

    const UINT sent = ::SendInput(static_cast<UINT>(inputs.size()), inputs.data(),
                                  sizeof(INPUT));
    if (sent != inputs.size()) {
        // The usual cause is an elevated window holding the foreground, which
        // blocks injection from an unelevated process entirely.
        MACTAB_WARN("desktops: SendInput sent %u of %zu events (err %lu)",
                    sent, inputs.size(), ::GetLastError());
        return false;
    }
    return true;
}

} // namespace

bool DesktopIdOf(HWND hwnd, GUID& out) {
    IVirtualDesktopManager* manager = Manager();
    if (!manager || !hwnd) return false;

    GUID id{};
    if (FAILED(manager->GetWindowDesktopId(hwnd, &id))) return false;

    out = id;
    return true;
}

int IndexOf(const State& state, const GUID& id) {
    for (size_t i = 0; i < state.all.size(); ++i)
        if (::IsEqualGUID(state.all[i].id, id))
            return static_cast<int>(i);
    return -1;
}

State Query(HWND ownWindow) {
    State state;

    std::vector<BYTE> raw;
    if (!ReadBinary(kRoot, L"VirtualDesktopIDs", raw) || raw.size() < sizeof(GUID)) {
        MACTAB_DIAG("desktops: no VirtualDesktopIDs in the registry");
        return state;
    }

    const size_t count = raw.size() / sizeof(GUID);
    state.all.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Desktop desktop;
        std::memcpy(&desktop.id, raw.data() + i * sizeof(GUID), sizeof(GUID));
        desktop.name = NameOf(desktop.id, static_cast<int>(i));
        state.all.push_back(std::move(desktop));
    }
    state.known = true;

    // Which one is being viewed. Three sources, in order of directness.
    std::vector<BYTE> currentRaw;
    GUID current{};
    bool haveCurrent = false;

    if ((ReadBinary(kRoot, L"CurrentVirtualDesktop", currentRaw) ||
         ReadBinary(SessionKeyPath(), L"CurrentVirtualDesktop", currentRaw)) &&
        currentRaw.size() >= sizeof(GUID)) {
        std::memcpy(&current, currentRaw.data(), sizeof(GUID));
        haveCurrent = true;
    }

    // Neither key exists until the user has switched desktops at least once in
    // this logon session, which on a freshly booted machine is most of the time.
    // A window with no parent is always on the desktop being viewed, so our own
    // host window answers the question directly.
    if (!haveCurrent && ownWindow)
        haveCurrent = DesktopIdOf(ownWindow, current);

    state.current = haveCurrent ? IndexOf(state, current) : -1;

    // A GUID that is not in the list means the window is pinned to every
    // desktop, which our own window should never be, so this is a genuine
    // inconsistency rather than an expected case.
    if (haveCurrent && state.current < 0) {
        MACTAB_WARN("desktops: current desktop %s is not in the ordered list",
                    ToUtf8(GuidToBraces(current)).c_str());
    }

    return state;
}

bool MoveWindowTo(HWND hwnd, const GUID& desktop) {
    if (!hwnd) return false;

    DWORD owner = 0;
    ::GetWindowThreadProcessId(hwnd, &owner);

    if (owner == ::GetCurrentProcessId()) {
        IVirtualDesktopManager* manager = Manager();
        if (!manager) return false;

        const HRESULT hr = manager->MoveWindowToDesktop(hwnd, desktop);
        if (FAILED(hr)) {
            MACTAB_WARN("desktops: cannot move our own window %p (0x%08lX)",
                        static_cast<void*>(hwnd), static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }

    // The view belongs to the top-level window, not to whatever child a caller
    // happened to be holding.
    HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;

    const HRESULT hr = CallShell([&](IShellDesktops* desktops) {
        return MoveThroughShell(desktops, root, desktop);
    });
    if (FAILED(hr)) {
        MACTAB_WARN("desktops: %p was not moved (0x%08lX)", static_cast<void*>(root),
                    static_cast<unsigned long>(hr));
        return false;
    }

    MACTAB_DIAG("desktops: moved %p to %s", static_cast<void*>(root),
                ToUtf8(GuidToBraces(desktop)).c_str());
    return true;
}

bool SwitchToId(const GUID& id) {
    const HRESULT hr = CallShell([&](IShellDesktops* desktops) {
        ComPtr<IShellDesktop> desktop;
        const HRESULT found = FindShellDesktop(desktops, id, desktop);
        return FAILED(found) ? found : desktops->SwitchDesktop(desktop.Get());
    });
    if (FAILED(hr)) {
        MACTAB_DIAG("desktops: the shell did not switch (0x%08lX)",
                    static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool CanMoveWindows() { return ShellDesktops() != nullptr; }

void ResetShell() { g_shell = Shell{}; }

bool SwitchTo(const State& state, int targetIndex) {
    if (!state.known || state.current < 0) return false;
    if (targetIndex < 0 || targetIndex >= static_cast<int>(state.all.size())) return false;

    const int delta = targetIndex - state.current;
    if (delta == 0) return true;

    const WORD key = (delta > 0) ? VK_RIGHT : VK_LEFT;
    const int  steps = (delta > 0) ? delta : -delta;

    MACTAB_DIAG("desktops: switching %d -> %d (%d step(s))",
                state.current, targetIndex, steps);

    for (int i = 0; i < steps; ++i)
        if (!SendChord(key)) return false;

    return true;
}

bool WaitForCurrent(const GUID& target, DWORD timeoutMs) {
    // Elapsed by unsigned subtraction rather than against a deadline. Adding the
    // timeout to the tick count wraps every 49.7 days, and a wrapped deadline is
    // already in the past, so the first check would time out at once and the
    // close would be refused for no reason anybody could work out.
    const DWORD started = ::GetTickCount();

    for (;;) {
        GUID current{};
        if (CurrentFromRegistry(current) && ::IsEqualGUID(current, target))
            return true;

        if (::GetTickCount() - started >= timeoutMs) return false;
        ::Sleep(10);
    }
}

bool CloseAt(HWND ownWindow, int index) {
    const State state = Query(ownWindow);
    if (!state.known || index < 0 || index >= static_cast<int>(state.all.size()))
        return false;

    // The shell ignores the chord on the last desktop, and offering it looks
    // broken. Callers hide the affordance; this is the backstop.
    if (state.all.size() <= 1) return false;

    if (index != state.current) {
        // Ctrl+Win+F4 closes the desktop being VIEWED and only that one, so the
        // view has to go there first.
        //
        // And it has to actually arrive before the close is sent. The chords go
        // through one input queue in order, so back to back they usually work,
        // but "usually" is not a standard to hold destructive things to: a
        // dropped chord, an elevated window holding the foreground, or the shell
        // running late would put the close on the wrong desktop. So the switch
        // is confirmed against the shell's own record of where the view is, and
        // if that never says what it should, nothing is closed at all.
        if (!SwitchTo(state, index)) return false;

        if (!WaitForCurrent(state.all[static_cast<size_t>(index)].id, 600)) {
            MACTAB_WARN("desktops: the view never reached %d, so nothing was closed",
                        index);
            return false;
        }
    }

    return CloseCurrent();
}

bool Create() {
    MACTAB_DIAG("desktops: creating a desktop");
    return SendChord('D');
}

bool CloseCurrent() {
    MACTAB_DIAG("desktops: closing the current desktop");
    return SendChord(VK_F4);
}

void LogState(HWND ownWindow) {
    if (!diag::Enabled()) return;

    const State state = Query(ownWindow);
    if (!state.known) {
        MACTAB_DIAG("desktops: unavailable");
        return;
    }

    MACTAB_DIAG("desktops: %zu, current %d", state.all.size(), state.current);
    for (size_t i = 0; i < state.all.size(); ++i)
        MACTAB_DIAG("  [%zu]%s %s %s", i,
                    (static_cast<int>(i) == state.current) ? " *" : "  ",
                    ToUtf8(state.all[i].name).c_str(),
                    ToUtf8(GuidToBraces(state.all[i].id)).c_str());
}

} // namespace mactab::desktops
