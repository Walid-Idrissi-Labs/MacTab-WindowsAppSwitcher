#include "pch.h"
#include <shobjidl.h>

#include "desktops.h"
#include "com.h"
#include "common.h"
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
    IVirtualDesktopManager* manager = Manager();
    if (!manager || !hwnd) return false;

    const HRESULT hr = manager->MoveWindowToDesktop(hwnd, desktop);
    if (FAILED(hr)) {
        MACTAB_DIAG("desktops: cannot move %p (0x%08lX); the public API only "
                    "moves this process's own windows",
                    static_cast<void*>(hwnd), static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

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
