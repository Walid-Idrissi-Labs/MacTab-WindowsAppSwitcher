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
    L"GroupByApp=1\r\n"
    L"\r\n"
    L"; Which display the panel opens on.\r\n"
    L";   active = the display holding the window you are currently in\r\n"
    L";   mouse  = the display the mouse pointer is on\r\n"
    L";   main   = always the main display\r\n"
    L"; Also settable from the tray menu.\r\n"
    L"PanelDisplay=active\r\n"
    L"\r\n"
    L"; Bend the desktop at the panel's rim, the way a real pane of glass would.\r\n"
    L"; Set to 0 if the edge of the panel looks doubled or smeared on your\r\n"
    L"; machine; everything else about the glass stays as it is.\r\n"
    L"GlassRefraction=1\r\n";

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

const wchar_t* PanelDisplayKeyword(PanelDisplay display) {
    switch (display) {
        case PanelDisplay::Mouse:   return L"mouse";
        case PanelDisplay::Primary: return L"main";
        default:                    return L"active";
    }
}

PanelDisplay ParsePanelDisplay(const std::wstring& text) {
    if (text == L"mouse") return PanelDisplay::Mouse;
    if (text == L"main" || text == L"primary") return PanelDisplay::Primary;
    return PanelDisplay::ActiveWindow;
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
    g_settings.panelDisplay = ParsePanelDisplay(ReadString(L"PanelDisplay", L"active"));
    g_settings.glassRefraction = ReadInt(L"GlassRefraction", 1) != 0;

    MACTAB_DIAG("config: revealDelay %u ms, leftAltOnly %d, tile %d, theme %s, "
                "groupByApp %d, panelDisplay %s, glassRefraction %d",
                g_settings.revealDelayMs, g_settings.leftAltOnly ? 1 : 0,
                g_settings.tileSize, ToUtf8(g_settings.theme).c_str(),
                g_settings.groupByApp ? 1 : 0,
                ToUtf8(PanelDisplayKeyword(g_settings.panelDisplay)).c_str(),
                g_settings.glassRefraction ? 1 : 0);
}

bool SetPanelDisplay(PanelDisplay display) {
    // Apply first, persist second. Failing to write is a reason to lose the
    // setting on the next launch, not a reason to ignore what the user just
    // clicked, and that has to hold for the no-settings-file case too.
    g_settings.panelDisplay = display;

    if (g_settingsPath.empty()) {
        MACTAB_WARN("config: no settings file, PanelDisplay will not survive a restart");
        return false;
    }

    const BOOL ok = ::WritePrivateProfileStringW(kSection, L"PanelDisplay",
                                                 PanelDisplayKeyword(display),
                                                 g_settingsPath.c_str());

    MACTAB_DIAG("config: panelDisplay -> %s (%s)",
                ToUtf8(PanelDisplayKeyword(display)).c_str(), ok ? "saved" : "not saved");
    return ok != FALSE;
}

bool SetTheme(const wchar_t* value) {
    if (!value) return false;

    g_settings.theme = value;   // same order, same reason, see SetPanelDisplay

    if (g_settingsPath.empty()) {
        MACTAB_WARN("config: no settings file, Theme will not survive a restart");
        return false;
    }

    const BOOL ok = ::WritePrivateProfileStringW(kSection, L"Theme", value,
                                                 g_settingsPath.c_str());

    MACTAB_DIAG("config: theme -> %s (%s)", ToUtf8(value).c_str(),
                ok ? "saved" : "not saved");
    return ok != FALSE;
}

// Must match AppId in installer/MacTab.iss. Inno appends _is1.
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
    L"{E2C26C45-D5E7-4EFD-A956-4168F7C3E0D6}_is1";

std::wstring UninstallCommand() {
    // HKCU first: per-user is the default install. A per-machine install (the
    // uiAccess route, if a certificate ever appears) writes HKLM instead.
    for (const HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        wchar_t value[MAX_PATH * 2] = L"";
        DWORD size = sizeof(value);
        if (::RegGetValueW(root, kUninstallKey, L"UninstallString",
                           RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS &&
            value[0] != L'\0') {
            return value;
        }
    }
    return {};
}

bool RunUninstaller() {
    std::wstring command = UninstallCommand();
    if (command.empty()) {
        MACTAB_WARN("config: no uninstall entry, this is not an installed copy");
        return false;
    }

    // The value is a quoted path, sometimes with arguments. ShellExecute wants
    // them separate, so split on the closing quote of the executable.
    std::wstring file = command;
    std::wstring args;
    if (!file.empty() && file.front() == L'"') {
        const size_t end = file.find(L'"', 1);
        if (end != std::wstring::npos) {
            args = file.substr(end + 1);
            file = file.substr(1, end - 1);
        }
    }

    while (!args.empty() && args.front() == L' ')
        args.erase(args.begin());

    // Inno's own convention for "show the wizard normally". Not /SILENT: the
    // user asked to uninstall from a tray menu, so they get the confirmation.
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", file.c_str(),
                                             args.empty() ? nullptr : args.c_str(),
                                             nullptr, SW_SHOWNORMAL);

    // ShellExecute returns a value <= 32 as an error code, not a handle.
    const bool ok = reinterpret_cast<INT_PTR>(result) > 32;
    MACTAB_DIAG("config: uninstaller %s (%s)", ToUtf8(file).c_str(),
                ok ? "started" : "failed to start");
    return ok;
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
