#include "pch.h"
#include "common.h"

namespace mactab {

std::string ToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

const std::wstring& AppDataDir() {
    // Resolved once; every later call is a cheap reference return.
    static const std::wstring dir = []() -> std::wstring {
        PWSTR raw = nullptr;
        if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)))
            return {};

        std::wstring path(raw);
        ::CoTaskMemFree(raw);
        path += L"\\MacTab";

        const int rc = ::SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
        if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS)
            return {};

        return path;
    }();
    return dir;
}

double NowMs() {
    static const double msPerTick = []() {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
    }();
    static const long long origin = []() {
        LARGE_INTEGER c{};
        ::QueryPerformanceCounter(&c);
        return c.QuadPart;
    }();

    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - origin) * msPerTick;
}

uint32_t WindowsBuildNumber() {
    static const uint32_t build = []() -> uint32_t {
        // GetVersionEx reports a shimmed version unless the manifest opts in,
        // and even then it is deprecated. RtlGetVersion always tells the truth.
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return 0;

        const auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
        if (!fn) return 0;

        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (fn(&info) != 0) return 0;

        return info.dwBuildNumber;
    }();
    return build;
}

} // namespace mactab
