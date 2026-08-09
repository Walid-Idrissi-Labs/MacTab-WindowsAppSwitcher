#include "pch.h"
#include "common.h"
#include "diag.h"

namespace mactab::diag {
namespace {

std::mutex   g_lock;
HANDLE       g_file    = INVALID_HANDLE_VALUE;
bool         g_enabled = false;
std::wstring g_path;

void WriteRaw(const char* bytes, size_t count) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(g_file, bytes, static_cast<DWORD>(count), &written, nullptr);
}

void WriteLine(const char* level, const char* message) {
    SYSTEMTIME t{};
    ::GetLocalTime(&t);

    char line[1400];
    const int n = std::snprintf(
        line, sizeof(line), "%02u:%02u:%02u.%03u [%-4s] [t%lu] %s\r\n",
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
        level, ::GetCurrentThreadId(), message);

    if (n > 0)
        WriteRaw(line, static_cast<size_t>((std::min)(n, static_cast<int>(sizeof(line)) - 1)));
}

} // namespace

void Init(bool enabled) {
    std::lock_guard<std::mutex> guard(g_lock);

    g_enabled = false;
    if (!enabled) return;

    const std::wstring& dir = AppDataDir();
    if (dir.empty()) return;   // nowhere to write; stay silent rather than crash

    g_path = dir + L"\\diag.log";

    // CREATE_ALWAYS: each run starts a fresh log, so a pasted log is always
    // one session. Full sharing so the file can be tailed while we run.
    g_file = ::CreateFileW(g_path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return;

    g_enabled = true;

    // UTF-8 BOM so Notepad and friends do not mangle non-ASCII window titles.
    WriteRaw("\xEF\xBB\xBF", 3);

    SYSTEMTIME t{};
    ::GetLocalTime(&t);

    char header[512];
    std::snprintf(header, sizeof(header),
                  "MacTab " MACTAB_VERSION "  |  session started %04u-%02u-%02u %02u:%02u:%02u"
                  "  |  Windows build %u  |  pid %lu",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                  WindowsBuildNumber(), ::GetCurrentProcessId());
    WriteLine("BOOT", header);
}

void Shutdown() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        WriteLine("EXIT", "session ended");
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_enabled = false;
}

bool Enabled() {
    return g_enabled;
}

const std::wstring& LogPath() {
    return g_path;
}

void Writef(const char* level, const char* fmt, ...) {
    char message[1200];

    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (n < 0) return;

    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_enabled) return;
    WriteLine(level, message);
}

ScopedTimer::ScopedTimer(const char* label)
    : m_label(label), m_startMs(Enabled() ? NowMs() : 0.0) {}

ScopedTimer::~ScopedTimer() {
    if (!Enabled()) return;
    Writef("TIME", "%s: %.2f ms", m_label, NowMs() - m_startMs);
}

} // namespace mactab::diag
