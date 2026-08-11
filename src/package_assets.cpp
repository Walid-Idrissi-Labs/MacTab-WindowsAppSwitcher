#include "pch.h"
#include <appmodel.h>
#include <shlwapi.h>
#include <xmllite.h>

#include "package_assets.h"
#include "com.h"
#include "common.h"
#include "diag.h"

namespace mactab::packages {
namespace {

// The nominal size of the logo the manifest names, used to turn a scale
// qualifier into a pixel count: scale-200 of a 44px logo is 88px.
constexpr int kNominalLogoPixels = 44;

std::wstring Lowered(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& c : out)
        c = static_cast<wchar_t>(::towlower(c));
    return out;
}

// --- the package ------------------------------------------------------------

// Every installed package in the family, not just one. Store apps are split
// into a main package plus resource packages, and the larger scale assets
// frequently live in one of the resource packages rather than the main one.
std::vector<std::wstring> PackagePaths(const std::wstring& family) {
    std::vector<std::wstring> paths;

    UINT32 count = 0, bufferLength = 0;
    if (::GetPackagesByPackageFamily(family.c_str(), &count, nullptr,
                                     &bufferLength, nullptr) != ERROR_INSUFFICIENT_BUFFER)
        return paths;
    if (count == 0) return paths;

    std::vector<PWSTR>  names(count);
    std::vector<wchar_t> buffer(bufferLength);
    if (::GetPackagesByPackageFamily(family.c_str(), &count, names.data(),
                                     &bufferLength, buffer.data()) != ERROR_SUCCESS)
        return paths;

    for (UINT32 i = 0; i < count; ++i) {
        UINT32 length = 0;
        if (::GetPackagePathByFullName(names[i], &length, nullptr) != ERROR_INSUFFICIENT_BUFFER)
            continue;

        std::wstring path(length, L'\0');
        if (::GetPackagePathByFullName(names[i], &length, path.data()) != ERROR_SUCCESS)
            continue;

        path.resize(length == 0 ? 0 : length - 1);   // drop the terminator
        if (!path.empty()) paths.push_back(std::move(path));
    }

    return paths;
}

// --- the manifest -----------------------------------------------------------

// #AARRGGBB, #RRGGBB, or "transparent". The schema also allows a list of
// predefined colour names, which are rare enough in real manifests to be worth
// leaving alone rather than shipping a name table for.
uint32_t ParseBackgroundColour(const std::wstring& text) {
    if (text.empty() || text[0] != L'#') return 0;

    unsigned value = 0;
    for (size_t i = 1; i < text.size(); ++i) {
        const wchar_t c = text[i];
        int digit;
        if (c >= L'0' && c <= L'9')      digit = c - L'0';
        else if (c >= L'a' && c <= L'f') digit = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') digit = c - L'A' + 10;
        else return 0;
        value = value * 16 + static_cast<unsigned>(digit);
    }

    const size_t digits = text.size() - 1;
    if (digits == 6) return 0xFF000000u | value;
    if (digits == 8) return value | 0xFF000000u;   // alpha is always ignored here
    return 0;
}

bool ReadAttribute(IXmlReader* reader, const wchar_t* name, std::wstring& out) {
    if (reader->MoveToAttributeByName(name, nullptr) != S_OK) {
        reader->MoveToElement();
        return false;
    }

    PCWSTR value = nullptr;
    const bool ok = SUCCEEDED(reader->GetValue(&value, nullptr)) && value;
    if (ok) out = value;

    reader->MoveToElement();
    return ok;
}

// Pull the named application's logo path and background colour out of
// AppxManifest.xml.
//
// Element names are compared without their prefix: the visual elements live in
// a uap namespace whose prefix and version have changed several times across
// manifest schema revisions, and matching on the local name is stable across
// all of them where matching on `uap:VisualElements` is not.
bool ReadManifest(const std::wstring& packagePath, const std::wstring& appId,
                  std::wstring& logo, uint32_t& background) {
    const std::wstring manifest = packagePath + L"\\AppxManifest.xml";

    ComPtr<IStream> stream;
    if (FAILED(::SHCreateStreamOnFileEx(manifest.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                                        FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                        stream.Put())))
        return false;

    ComPtr<IXmlReader> reader;
    if (FAILED(::CreateXmlReader(__uuidof(IXmlReader), reader.PutVoid(), nullptr)) ||
        FAILED(reader->SetInput(stream.Get())))
        return false;

    bool inThisApp = false;
    XmlNodeType type;

    while (reader->Read(&type) == S_OK) {
        if (type != XmlNodeType_Element) continue;

        PCWSTR local = nullptr;
        if (FAILED(reader->GetLocalName(&local, nullptr)) || !local) continue;

        if (::wcscmp(local, L"Application") == 0) {
            std::wstring id;
            inThisApp = ReadAttribute(reader.Get(), L"Id", id) && id == appId;
            continue;
        }

        if (!inThisApp || ::wcscmp(local, L"VisualElements") != 0)
            continue;

        std::wstring colour;
        if (ReadAttribute(reader.Get(), L"BackgroundColor", colour))
            background = ParseBackgroundColour(colour);

        // Square44x44Logo is what the taskbar draws from. Square150x150Logo is
        // the Start tile, which is a different picture with tile padding built
        // into it, and only worth having when there is no 44 at all.
        return ReadAttribute(reader.Get(), L"Square44x44Logo", logo) ||
               ReadAttribute(reader.Get(), L"Square150x150Logo", logo);
    }

    return false;
}

// --- variant selection ------------------------------------------------------

struct Candidate {
    std::wstring path;
    int          score = -1;
};

// Score one asset's qualifiers.
//
// Resolution dominates, because the whole reason for coming here is that the
// shell would not give us a big enough icon. Between two of the same size, the
// unplated design wins: it is the one drawn to stand on its own, and it is what
// the taskbar shows.
//
// Returns -1 to reject. High-contrast variants are rejected outright; they are
// flat black or flat white and would look like a mistake next to everything
// else in the panel.
int ScoreQualifiers(const std::wstring& qualifiers) {
    int pixels = kNominalLogoPixels;
    int form   = 1;     // plated or unspecified
    int penalty = 0;

    size_t start = 0;
    while (start <= qualifiers.size()) {
        const size_t end = qualifiers.find(L'_', start);
        const std::wstring token =
            Lowered(qualifiers.substr(start, end == std::wstring::npos
                                                 ? std::wstring::npos : end - start));
        start = (end == std::wstring::npos) ? qualifiers.size() + 1 : end + 1;
        if (token.empty()) continue;

        const size_t dash = token.find(L'-');
        if (dash == std::wstring::npos) continue;

        const std::wstring name  = token.substr(0, dash);
        const std::wstring value = token.substr(dash + 1);

        if (name == L"targetsize") {
            pixels = ::_wtoi(value.c_str());
        } else if (name == L"scale") {
            const int scale = ::_wtoi(value.c_str());
            if (scale > 0) pixels = kNominalLogoPixels * scale / 100;
        } else if (name == L"altform") {
            if (value == L"unplated")            form = 3;
            else if (value == L"lightunplated")  form = 2;
        } else if (name == L"contrast") {
            if (value != L"standard") return -1;
        } else if (name == L"lang" || name == L"language") {
            // A localised logo is still the app's logo, just not the one meant
            // for this install. Usable, but never in preference to the neutral
            // one sitting next to it.
            penalty += 1;
        }
    }

    if (pixels <= 0) return -1;
    return pixels * 16 + form * 4 - penalty;
}

// Whether `name` is `stem` plus qualifiers plus `extension`, and if so what the
// qualifiers were.
bool SplitVariant(const std::wstring& name, const std::wstring& stem,
                  const std::wstring& extension, std::wstring& qualifiers) {
    const std::wstring lowered = Lowered(name);
    const std::wstring lowStem = Lowered(stem);
    const std::wstring lowExt  = Lowered(extension);

    if (lowered.size() < lowStem.size() + lowExt.size()) return false;
    if (lowered.compare(0, lowStem.size(), lowStem) != 0) return false;
    if (lowered.compare(lowered.size() - lowExt.size(), lowExt.size(), lowExt) != 0)
        return false;

    const std::wstring middle =
        name.substr(lowStem.size(), name.size() - lowStem.size() - lowExt.size());

    if (middle.empty()) { qualifiers.clear(); return true; }
    if (middle[0] != L'.') return false;   // a different file that happens to share a prefix

    qualifiers = middle.substr(1);
    return true;
}

void CollectVariants(const std::wstring& directory, const std::wstring& stem,
                     const std::wstring& extension, Candidate& best) {
    auto offer = [&](const std::wstring& path, const std::wstring& qualifiers) {
        const int score = ScoreQualifiers(qualifiers);
        if (score > best.score) {
            best.score = score;
            best.path  = path;
        }
    };

    WIN32_FIND_DATAW found{};
    const HANDLE search = ::FindFirstFileW((directory + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;

    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") continue;

        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Qualifiers are also legal as directory names, so Assets\scale-200
            // holding a plain Square44x44Logo.png is a valid layout and does
            // turn up. One level down is as far as this goes; nesting them is
            // legal too but nobody does it.
            const std::wstring nested = directory + L"\\" + name + L"\\" + stem + extension;
            if (::GetFileAttributesW(nested.c_str()) != INVALID_FILE_ATTRIBUTES)
                offer(nested, name);
            continue;
        }

        std::wstring qualifiers;
        if (SplitVariant(name, stem, extension, qualifiers))
            offer(directory + L"\\" + name, qualifiers);
    } while (::FindNextFileW(search, &found));

    ::FindClose(search);
}

// Never split an AUMID by hand. This call is the definition of the format.
bool SplitAumid(const std::wstring& aumid, std::wstring& family, std::wstring& appId) {
    UINT32 familyLength = 0, appIdLength = 0;
    if (::ParseApplicationUserModelId(aumid.c_str(), &familyLength, nullptr,
                                      &appIdLength, nullptr) != ERROR_INSUFFICIENT_BUFFER)
        return false;

    family.assign(familyLength, L'\0');
    appId.assign(appIdLength, L'\0');
    if (::ParseApplicationUserModelId(aumid.c_str(), &familyLength, family.data(),
                                      &appIdLength, appId.data()) != ERROR_SUCCESS)
        return false;

    family.resize(familyLength == 0 ? 0 : familyLength - 1);
    appId.resize(appIdLength == 0 ? 0 : appIdLength - 1);
    return !family.empty();
}

} // namespace

std::wstring VersionTag(const std::wstring& aumid) {
    std::wstring family, appId;
    if (aumid.empty() || !SplitAumid(aumid, family, appId))
        return {};

    UINT32 count = 0, bufferLength = 0;
    if (::GetPackagesByPackageFamily(family.c_str(), &count, nullptr,
                                     &bufferLength, nullptr) != ERROR_INSUFFICIENT_BUFFER ||
        count == 0)
        return {};

    std::vector<PWSTR>   names(count);
    std::vector<wchar_t> buffer(bufferLength);
    if (::GetPackagesByPackageFamily(family.c_str(), &count, names.data(),
                                     &bufferLength, buffer.data()) != ERROR_SUCCESS)
        return {};

    std::wstring tag;
    for (UINT32 i = 0; i < count; ++i) {
        tag += names[i];
        tag += L';';
    }
    return tag;
}

bool FindLogo(const std::wstring& aumid, Logo& out) {
    if (aumid.empty()) return false;

    std::wstring family, appId;
    if (!SplitAumid(aumid, family, appId))
        return false;

    const std::vector<std::wstring> paths = PackagePaths(family);
    if (paths.empty()) return false;

    // The manifest is in the main package; the assets it names may be in any
    // package of the family, so the relative path is resolved against each.
    std::wstring relative;
    uint32_t background = 0;
    for (const std::wstring& path : paths) {
        if (ReadManifest(path, appId, relative, background))
            break;
    }
    if (relative.empty()) {
        MACTAB_DIAG("packages: no logo in the manifest for %s", ToUtf8(aumid).c_str());
        return false;
    }

    for (wchar_t& c : relative)
        if (c == L'/') c = L'\\';

    const size_t slash = relative.find_last_of(L'\\');
    const std::wstring subdirectory = (slash == std::wstring::npos)
        ? std::wstring{} : relative.substr(0, slash);
    const std::wstring filename = (slash == std::wstring::npos)
        ? relative : relative.substr(slash + 1);

    const size_t dot = filename.find_last_of(L'.');
    const std::wstring stem = (dot == std::wstring::npos) ? filename : filename.substr(0, dot);
    const std::wstring extension = (dot == std::wstring::npos)
        ? std::wstring(L".png") : filename.substr(dot);

    Candidate best;
    for (const std::wstring& path : paths) {
        const std::wstring directory =
            subdirectory.empty() ? path : (path + L"\\" + subdirectory);
        CollectVariants(directory, stem, extension, best);
    }

    if (best.path.empty()) {
        MACTAB_DIAG("packages: manifest names %s but nothing matching is on disk",
                    ToUtf8(relative).c_str());
        return false;
    }

    out.path       = best.path;
    out.background = background;

    MACTAB_DIAG("packages: %s -> %s", ToUtf8(aumid).c_str(), ToUtf8(out.path).c_str());
    return true;
}

} // namespace mactab::packages
