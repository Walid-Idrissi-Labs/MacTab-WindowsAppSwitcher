#include "pch.h"
#include "tray.h"
#include "diag.h"

namespace mactab {
namespace {

// Only ever one tray icon, so a fixed id is fine.
constexpr UINT kIconId = 1;

// Copy into one of the shell's fixed-size wchar_t fields, always terminating.
// Hand-rolled rather than wcsncpy_s so the file stays compilable outside MSVC
// (see tools/syntax-check.sh).
template <size_t N>
void CopyFixed(wchar_t (&dest)[N], std::wstring_view src) {
    const size_t n = (std::min)(src.size(), N - 1);
    if (n) std::memcpy(dest, src.data(), n * sizeof(wchar_t));
    dest[n] = L'\0';
}

} // namespace

Tray::~Tray() {
    Destroy();
}

UINT Tray::TaskbarCreatedMessage() {
    static const UINT msg = ::RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

NOTIFYICONDATAW Tray::MakeData() const {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_owner;
    nid.uID              = kIconId;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = m_callbackMsg;
    nid.hIcon            = m_icon;

    CopyFixed(nid.szTip, m_tooltip);
    return nid;
}

bool Tray::Create(HINSTANCE instance, HWND owner, UINT callbackMsg, UINT iconResourceId,
                  const wchar_t* tooltip) {
    m_owner       = owner;
    m_callbackMsg = callbackMsg;
    m_tooltip     = tooltip ? tooltip : L"";

    // LoadImage with the small-icon metrics picks the correctly-sized entry out
    // of the .ico rather than scaling a 32px one down, which is visibly mushy.
    m_icon = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(iconResourceId), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    m_ownsIcon = (m_icon != nullptr);

    if (!m_icon) {
        MACTAB_WARN("tray: icon resource %u failed to load (err %lu), using system fallback",
                    iconResourceId, ::GetLastError());
        // Shared system icon — must NOT be destroyed.
        m_icon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }

    NOTIFYICONDATAW nid = MakeData();
    if (!::Shell_NotifyIconW(NIM_ADD, &nid)) {
        MACTAB_FAIL("tray: Shell_NotifyIcon(NIM_ADD) failed (err %lu)", ::GetLastError());
        return false;
    }

    // Opt into the v4 callback contract. Must happen after NIM_ADD.
    nid.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &nid);

    m_added = true;
    MACTAB_DIAG("tray: icon added");
    return true;
}

void Tray::Readd() {
    if (!m_owner) return;

    // Explorer restarted, so our previous registration is gone. A stray NIM_ADD
    // when one already exists is harmless.
    NOTIFYICONDATAW nid = MakeData();
    if (::Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &nid);
        m_added = true;
        MACTAB_DIAG("tray: icon re-added after Explorer restart");
    } else {
        MACTAB_WARN("tray: re-add failed (err %lu)", ::GetLastError());
    }
}

void Tray::ShowBalloon(const wchar_t* title, const wchar_t* text) {
    if (!m_added) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_owner;
    nid.uID    = kIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_NONE;

    CopyFixed(nid.szInfoTitle, title ? title : L"");
    CopyFixed(nid.szInfo, text ? text : L"");

    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::Destroy() {
    if (m_added) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = m_owner;
        nid.uID    = kIconId;
        ::Shell_NotifyIconW(NIM_DELETE, &nid);
        m_added = false;
    }

    if (m_icon && m_ownsIcon)
        ::DestroyIcon(m_icon);
    m_icon     = nullptr;
    m_ownsIcon = false;
    m_owner = nullptr;
}

} // namespace mactab
