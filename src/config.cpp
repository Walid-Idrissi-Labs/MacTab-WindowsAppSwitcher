#include "pch.h"
#include <wincodec.h>

#include "config.h"
#include "com.h"
#include "common.h"
#include "diag.h"

namespace mactab::config {
namespace {

constexpr wchar_t kSection[]  = L"MacTab";
constexpr wchar_t kRunKey[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MacTab";

Settings     g_settings;
std::wstring g_settingsPath;
std::wstring g_themesDir;

const wchar_t* kDefaultIni =
    L"; MacTab settings. Delete this file to restore defaults.\r\n"
    L"[MacTab]\r\n"
    L"; Milliseconds Alt must be held before the panel appears. A quicker tap\r\n"
    L"; switches to the previous app without showing anything, like macOS.\r\n"
    L"RevealDelayMs=180\r\n"
    L"\r\n"
    L"; Trigger on Left Alt only, and never while Ctrl is held. Keep this on if\r\n"
    L"; you use a layout where AltGr (Ctrl+RightAlt) types @ # { } [ ].\r\n"
    L"LeftAltOnly=1\r\n"
    L"\r\n"
    L"; Icon tile size in logical pixels at 100% scaling.\r\n"
    L"TileSize=128\r\n"
    L"\r\n"
    L"; auto | dark | light\r\n"
    L"Theme=auto\r\n"
    L"\r\n"
    L"; 1 = one tile per application (macOS). 0 = one tile per window.\r\n"
    L"GroupByApp=1\r\n";

std::wstring ReadString(const wchar_t* key, const wchar_t* fallback) {
    wchar_t buffer[128] = L"";
    ::GetPrivateProfileStringW(kSection, key, fallback, buffer, ARRAYSIZE(buffer),
                               g_settingsPath.c_str());
    return buffer;
}

int ReadInt(const wchar_t* key, int fallback) {
    return static_cast<int>(::GetPrivateProfileIntW(kSection, key, fallback,
                                                    g_settingsPath.c_str()));
}

// Executable stem, or a filesystem-safe form of the AUMID.
std::wstring ThemeName(const std::wstring& exePath, const std::wstring& aumid) {
    if (!aumid.empty()) {
        std::wstring safe = aumid;
        for (wchar_t& c : safe) {
            if (c == L'\\' || c == L'/' || c == L':' || c == L'!' ||
                c == L'*'  || c == L'?' || c == L'"' || c == L'<' ||
                c == L'>'  || c == L'|')
                c = L'_';
        }
        return safe;
    }

    const size_t slash = exePath.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? exePath : exePath.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        name.erase(dot);
    return name;
}

// Decode a PNG through WIC, which is in the OS and handles colour profiles and
// interlacing properly, worth far more than the few lines a minimal decoder
// would save.
Bitmap DecodeImageFile(const std::wstring& path) {
    ComApartment apartment(COINIT_APARTMENTTHREADED);

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.Put()))))
        return {};

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad,
                                                  decoder.Put())))
        return {};

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.Put())))
        return {};

    // Convert to straight-alpha BGRA to match our Bitmap contract; the pipeline
    // premultiplies only at upload.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.Put())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return {};

    UINT width = 0, height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
        return {};

    if (width > 2048 || height > 2048) {
        MACTAB_WARN("config: theme image %s is %ux%u, refusing",
                    ToUtf8(path).c_str(), width, height);
        return {};
    }

    Bitmap out = Bitmap::Create(static_cast<int>(width), static_cast<int>(height));
    const UINT stride = width * 4;
    const UINT bytes  = stride * height;

    if (FAILED(converter->CopyPixels(nullptr, stride, bytes,
                                     reinterpret_cast<BYTE*>(out.pixels.data()))))
        return {};

    return out;
}

} // namespace

const Settings& Current() { return g_settings; }

void Load() {
    const std::wstring& dir = AppDataDir();
    if (dir.empty()) {
        MACTAB_WARN("config: no app data directory; using built-in defaults");
        return;
    }

    g_settingsPath = dir + L"\\settings.ini";
    g_themesDir    = dir + L"\\themes";

    // Write the annotated default file on first run so the settings are
    // discoverable without documentation.
    if (::GetFileAttributesW(g_settingsPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const UniqueHandle file(::CreateFileW(g_settingsPath.c_str(), GENERIC_WRITE, 0,
                                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                              nullptr));
        if (file && file.get() != INVALID_HANDLE_VALUE) {
            // UTF-16LE with a BOM, which is what the profile APIs write.
            const wchar_t bom = 0xFEFF;
            DWORD written = 0;
            ::WriteFile(file.get(), &bom, sizeof(bom), &written, nullptr);
            ::WriteFile(file.get(), kDefaultIni,
                        static_cast<DWORD>(::lstrlenW(kDefaultIni) * sizeof(wchar_t)),
                        &written, nullptr);
        }
    }

    ::SHCreateDirectoryExW(nullptr, g_themesDir.c_str(), nullptr);

    g_settings.revealDelayMs = static_cast<UINT>(
        (std::max)(0, (std::min)(2000, ReadInt(L"RevealDelayMs", 180))));
    g_settings.leftAltOnly = ReadInt(L"LeftAltOnly", 1) != 0;
    g_settings.tileSize    = (std::max)(48, (std::min)(256, ReadInt(L"TileSize", 128)));
    g_settings.theme       = ReadString(L"Theme", L"auto");
    g_settings.groupByApp  = ReadInt(L"GroupByApp", 1) != 0;

    MACTAB_DIAG("config: revealDelay %u ms, leftAltOnly %d, tile %d, theme %s, groupByApp %d",
                g_settings.revealDelayMs, g_settings.leftAltOnly ? 1 : 0,
                g_settings.tileSize, ToUtf8(g_settings.theme).c_str(),
                g_settings.groupByApp ? 1 : 0);
}

bool AutostartEnabled() {
    wchar_t value[MAX_PATH * 2] = L"";
    DWORD size = sizeof(value);
    return ::RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue,
                          RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
}

bool SetAutostart(bool enabled) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;

    bool ok = false;
    if (enabled) {
        wchar_t exePath[MAX_PATH * 2] = L"";
        if (::GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath))) {
            // Quoted: the path very often contains spaces, and an unquoted Run
            // value is parsed at the first one.
            std::wstring command = L"\"";
            command += exePath;
            command += L"\"";

            ok = ::RegSetValueExW(key, kRunValue, 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(command.c_str()),
                                  static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
                 == ERROR_SUCCESS;
        }
    } else {
        const LSTATUS status = ::RegDeleteValueW(key, kRunValue);
        ok = (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND);
    }

    ::RegCloseKey(key);
    MACTAB_DIAG("config: autostart %s -> %s", enabled ? "enable" : "disable",
                ok ? "ok" : "failed");
    return ok;
}

const std::wstring& ThemesDir() { return g_themesDir; }

Bitmap LoadThemeOverride(const std::wstring& exePath, const std::wstring& aumid) {
    if (g_themesDir.empty()) return {};

    const std::wstring name = ThemeName(exePath, aumid);
    if (name.empty()) return {};

    for (const wchar_t* extension : { L".png", L".PNG" }) {
        const std::wstring path = g_themesDir + L"\\" + name + extension;
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;

        Bitmap image = DecodeImageFile(path);
        if (!image.Empty()) {
            MACTAB_DIAG("config: theme override for %s", ToUtf8(name).c_str());
            return image;
        }
        MACTAB_WARN("config: could not decode %s", ToUtf8(path).c_str());
    }

    return {};
}

} // namespace mactab::config
