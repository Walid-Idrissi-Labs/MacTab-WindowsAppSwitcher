#include "pch.h"
#include "app_identity.h"
#include "common.h"
#include "diag.h"

namespace mactab {
namespace {

// APPMODEL_ERROR_NO_APPLICATION is in appmodel.h, which is not uniformly
// available across toolchains; the value is stable and documented.
constexpr LONG kAppModelErrorNoApplication = 15703;

using GetApplicationUserModelIdFn =
    LONG(WINAPI*)(HANDLE process, UINT32* length, PWSTR id);

// Resolved once. Present on Windows 8 and later, so effectively always for us,
// but going through GetProcAddress keeps the import out of the binary and lets
// the code degrade instead of failing to load if it is ever absent.
GetApplicationUserModelIdFn GetApplicationUserModelIdProc() {
    static const GetApplicationUserModelIdFn fn = []() -> GetApplicationUserModelIdFn {
        const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        if (!kernel32) return nullptr;
        return reinterpret_cast<GetApplicationUserModelIdFn>(
            reinterpret_cast<void*>(::GetProcAddress(kernel32, "GetApplicationUserModelId")));
    }();
    return fn;
}

struct CacheEntry {
    FILETIME    creation{};   // guards against PID reuse
    AppIdentity identity;
};

std::unordered_map<DWORD, CacheEntry> g_cache;

std::wstring ToLower(std::wstring s) {
    // CharLowerBuffW is locale-aware and, unlike towlower in a loop, handles
    // the full UTF-16 range the shell can hand us.
    if (!s.empty())
        ::CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring ClassNameOf(HWND hwnd) {
    wchar_t buffer[128] = L"";
    const int n = ::GetClassNameW(hwnd, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, static_cast<size_t>(n > 0 ? n : 0));
}

BOOL CALLBACK FindCoreWindowProc(HWND child, LPARAM param) {
    if (ClassNameOf(child) == L"Windows.UI.Core.CoreWindow") {
        *reinterpret_cast<HWND*>(param) = child;
        return FALSE;   // stop enumerating
    }
    return TRUE;
}

// For a UWP frame window, find the child that actually belongs to the app.
HWND FindCoreWindow(HWND frame) {
    HWND found = nullptr;
    ::EnumChildWindows(frame, FindCoreWindowProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool ProcessCreationTime(HANDLE process, FILETIME& creation) {
    FILETIME exitTime{}, kernelTime{}, userTime{};
    return ::GetProcessTimes(process, &creation, &exitTime, &kernelTime, &userTime) != FALSE;
}

std::wstring ProcessImagePath(HANDLE process) {
    // QueryFullProcessImageName works with PROCESS_QUERY_LIMITED_INFORMATION,
    // which we can get for most processes at our own integrity level.
    // GetModuleFileNameEx would need PROCESS_VM_READ and fails far more often.
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = ARRAYSIZE(buffer);
    if (::QueryFullProcessImageNameW(process, 0, buffer, &size))
        return std::wstring(buffer, size);
    return {};
}

std::wstring PackageAumid(HANDLE process) {
    const auto fn = GetApplicationUserModelIdProc();
    if (!fn) return {};

    UINT32 length = 0;
    const LONG probe = fn(process, &length, nullptr);

    // Not a packaged app, by far the common case, and not an error.
    if (probe == kAppModelErrorNoApplication || length == 0)
        return {};

    std::wstring aumid(length, L'\0');
    if (fn(process, &length, aumid.data()) != ERROR_SUCCESS)
        return {};

    // length includes the terminator on success.
    if (length > 0 && aumid[length - 1] == L'\0')
        aumid.resize(length - 1);
    else
        aumid.resize(length);

    return aumid;
}

// FileDescription from the version resource; this is what Explorer shows as a
// program's name, and it is what users expect on the switcher label
// ("Google Chrome", not "chrome").
std::wstring VersionFileDescription(const std::wstring& exePath) {
    if (exePath.empty()) return {};

    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(exePath.c_str(), &ignored);
    if (size == 0) return {};

    std::vector<BYTE> block(size);
    if (!::GetFileVersionInfoW(exePath.c_str(), 0, size, block.data()))
        return {};

    // Use whichever translation the file actually ships rather than assuming
    // US English; plenty of apps are localised or use a neutral codepage.
    struct LangCodepage { WORD language; WORD codePage; };
    LangCodepage* translations = nullptr;
    UINT translationBytes = 0;
    if (!::VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
                          reinterpret_cast<LPVOID*>(&translations), &translationBytes) ||
        translationBytes < sizeof(LangCodepage) || !translations) {
        return {};
    }

    const size_t count = translationBytes / sizeof(LangCodepage);
    for (size_t i = 0; i < count; ++i) {
        wchar_t subBlock[64];
        ::wsprintfW(subBlock, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                    translations[i].language, translations[i].codePage);

        wchar_t* value = nullptr;
        UINT valueChars = 0;
        if (::VerQueryValueW(block.data(), subBlock,
                             reinterpret_cast<LPVOID*>(&value), &valueChars) &&
            value && valueChars > 0) {
            std::wstring description(value, valueChars);
            while (!description.empty() && description.back() == L'\0')
                description.pop_back();
            if (!description.empty())
                return description;
        }
    }
    return {};
}

std::wstring FileStem(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);

    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        name.erase(dot);

    return name;
}

AppIdentity BuildIdentity(HANDLE process) {
    AppIdentity identity;

    identity.exePath = ProcessImagePath(process);
    identity.aumid   = PackageAumid(process);

    if (!identity.aumid.empty()) {
        identity.packaged = true;
        identity.key      = ToLower(identity.aumid);
        // displayName stays empty: the friendly name lives in the package
        // manifest and needs the shell to read. The icon worker fills it in;
        // callers fall back to the window title meanwhile.
    } else {
        identity.key         = ToLower(identity.exePath);
        identity.displayName = VersionFileDescription(identity.exePath);
        if (identity.displayName.empty())
            identity.displayName = FileStem(identity.exePath);
    }

    return identity;
}

} // namespace

const AppIdentity* ResolveApp(HWND hwnd) {
    if (!hwnd) return nullptr;

    // A UWP window's frame belongs to ApplicationFrameHost; the app itself owns
    // the CoreWindow child. Grouping on the frame would merge every Store app
    // into a single "app".
    HWND identityWindow = hwnd;
    if (ClassNameOf(hwnd) == L"ApplicationFrameWindow") {
        if (const HWND core = FindCoreWindow(hwnd))
            identityWindow = core;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(identityWindow, &pid);
    if (pid == 0) return nullptr;

    const UniqueHandle process(
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        // Usually a higher-integrity process. Expected, not alarming.
        MACTAB_WARN("identity: cannot open pid %lu (err %lu)", pid, ::GetLastError());
        return nullptr;
    }

    FILETIME creation{};
    const bool haveCreation = ProcessCreationTime(process.get(), creation);

    const auto cached = g_cache.find(pid);
    if (cached != g_cache.end()) {
        // Same PID and same start time means genuinely the same process.
        const bool sameProcess =
            haveCreation &&
            cached->second.creation.dwLowDateTime  == creation.dwLowDateTime &&
            cached->second.creation.dwHighDateTime == creation.dwHighDateTime;

        if (sameProcess)
            return &cached->second.identity;

        g_cache.erase(cached);   // PID was reused
    }

    AppIdentity identity = BuildIdentity(process.get());
    if (identity.key.empty()) {
        MACTAB_WARN("identity: pid %lu produced no usable key", pid);
        return nullptr;
    }

    CacheEntry entry;
    entry.creation = creation;
    entry.identity = std::move(identity);

    const auto [inserted, ok] = g_cache.insert_or_assign(pid, std::move(entry));
    (void)ok;
    return &inserted->second.identity;
}

void ClearIdentityCache() {
    g_cache.clear();
}

} // namespace mactab
