#include "pch.h"
// shobjidl.h rather than shobjidl_core.h: MSVC's shobjidl.h includes the core
// header, but mingw only ships the former, and this file is worth keeping in
// the off-Windows syntax check.
#include <shobjidl.h>
#include <condition_variable>
#include <list>
#include <thread>

#include "icons.h"
#include "app_identity.h"
#include "com.h"
#include "config.h"
#include "common.h"
#include "diag.h"
#include "icon_source.h"
#include "squircle.h"

namespace mactab::icons {
namespace {

// Ask the shell for this, regardless of the final tile size. Shell icon caches
// hold a 256px "jumbo" entry, and starting from it means downscaling (which is
// clean) rather than upscaling a 32px icon (which is mush).
constexpr int kSourceIconSize = 256;

// Bounded so a long session with many apps cannot grow without limit. At 128px
// that is roughly 6 MB of tiles, comfortably inside the memory budget.
constexpr size_t kMaxCachedTiles = 96;

constexpr uint32_t kDiskCacheMagic   = 0x4D544943;   // 'MTIC'
constexpr uint32_t kDiskCacheVersion = 1;

struct CacheKey {
    std::wstring appKey;
    int          size = 0;

    bool operator==(const CacheKey& other) const {
        return size == other.size && appKey == other.appKey;
    }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& key) const {
        return std::hash<std::wstring>{}(key.appKey) ^ (static_cast<size_t>(key.size) * 2654435761u);
    }
};

// --- shared state -----------------------------------------------------------
std::mutex                     g_lock;
std::condition_variable        g_wake;
std::deque<Request>            g_queue;
bool                           g_stopping = false;

std::unordered_map<CacheKey, Bitmap, CacheKeyHash> g_tiles;
std::list<CacheKey>                                g_lru;      // front = most recent
std::unordered_map<std::wstring, std::wstring>     g_displayNames;

// Requests already queued or completed, so a repeated Acquire during a gesture
// does not enqueue the same work several times per second.
std::unordered_map<CacheKey, bool, CacheKeyHash> g_inFlight;

std::thread g_worker;
HWND        g_notifyWindow  = nullptr;
UINT        g_notifyMessage = 0;

// --- helpers ----------------------------------------------------------------

uint64_t Fnv1a(const void* data, size_t bytes, uint64_t seed = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Cache identity includes the source file's size and write time, so an app
// update invalidates its cached tile without any explicit versioning.
std::wstring DiskCachePath(const Request& request) {
    const std::wstring& dir = AppDataDir();
    if (dir.empty()) return {};

    uint64_t hash = Fnv1a(request.key.data(), request.key.size() * sizeof(wchar_t));
    hash = Fnv1a(&request.size, sizeof(request.size), hash);

    if (!request.exePath.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (::GetFileAttributesExW(request.exePath.c_str(), GetFileExInfoStandard, &attributes)) {
            hash = Fnv1a(&attributes.ftLastWriteTime, sizeof(attributes.ftLastWriteTime), hash);
            hash = Fnv1a(&attributes.nFileSizeLow,    sizeof(attributes.nFileSizeLow),    hash);
        }
    }

    wchar_t name[64];
    ::wsprintfW(name, L"\\iconcache\\%08X%08X.tile",
                static_cast<unsigned>(hash >> 32), static_cast<unsigned>(hash & 0xFFFFFFFFu));
    return dir + name;
}

bool ReadDiskCache(const std::wstring& path, Bitmap& out) {
    if (path.empty()) return false;

    const UniqueHandle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                          nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                          nullptr));
    if (!file || file.get() == INVALID_HANDLE_VALUE) return false;

    uint32_t header[4]{};
    DWORD read = 0;
    if (!::ReadFile(file.get(), header, sizeof(header), &read, nullptr) || read != sizeof(header))
        return false;
    if (header[0] != kDiskCacheMagic || header[1] != kDiskCacheVersion)
        return false;

    const int width  = static_cast<int>(header[2]);
    const int height = static_cast<int>(header[3]);
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024)
        return false;

    Bitmap bitmap = Bitmap::Create(width, height);
    const DWORD expected = static_cast<DWORD>(bitmap.pixels.size() * sizeof(uint32_t));
    if (!::ReadFile(file.get(), bitmap.pixels.data(), expected, &read, nullptr) || read != expected)
        return false;

    out = std::move(bitmap);
    return true;
}

void WriteDiskCache(const std::wstring& path, const Bitmap& bitmap) {
    if (path.empty() || bitmap.Empty()) return;

    // The directory is created lazily; ignore "already exists".
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        ::SHCreateDirectoryExW(nullptr, path.substr(0, slash).c_str(), nullptr);

    const UniqueHandle file(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file || file.get() == INVALID_HANDLE_VALUE) return;

    const uint32_t header[4] = {
        kDiskCacheMagic, kDiskCacheVersion,
        static_cast<uint32_t>(bitmap.width), static_cast<uint32_t>(bitmap.height)
    };

    DWORD written = 0;
    ::WriteFile(file.get(), header, sizeof(header), &written, nullptr);
    ::WriteFile(file.get(), bitmap.pixels.data(),
                static_cast<DWORD>(bitmap.pixels.size() * sizeof(uint32_t)), &written, nullptr);
}

// --- shell extraction (worker thread only) ----------------------------------

Bitmap ImageFromShellItem(IShellItem* item, int size) {
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(factory.Put()))))
        return {};

    HBITMAP bitmap = nullptr;
    const SIZE requested{ size, size };

    // ICONONLY: never substitute a document thumbnail for the app's icon.
    // BIGGERSIZEOK: prefer a larger cached entry over an upscaled small one.
    const HRESULT hr = factory->GetImage(
        requested, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap);
    if (FAILED(hr) || !bitmap)
        return {};

    Bitmap result = FromHBitmap(bitmap);
    ::DeleteObject(bitmap);
    return result;
}

std::wstring ShellItemDisplayName(IShellItem* item) {
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &raw)) || !raw)
        return {};

    std::wstring name(raw);
    ::CoTaskMemFree(raw);
    return name;
}

// Packaged apps live in the virtual Apps folder, addressed by AUMID. This is
// also the only place their friendly display name and their real Store logo
// can be read from.
Bitmap ExtractPackaged(const Request& request, std::wstring& displayName) {
    if (request.aumid.empty()) return {};

    const std::wstring parsingName = L"shell:AppsFolder\\" + request.aumid;

    ComPtr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(parsingName.c_str(), nullptr,
                                             IID_PPV_ARGS(item.Put())))) {
        MACTAB_WARN("icons: could not bind %s", ToUtf8(parsingName).c_str());
        return {};
    }

    displayName = ShellItemDisplayName(item.Get());
    return ImageFromShellItem(item.Get(), kSourceIconSize);
}

Bitmap ExtractExecutable(const Request& request) {
    if (request.exePath.empty()) return {};

    ComPtr<IShellItem> item;
    if (FAILED(::SHCreateItemFromParsingName(request.exePath.c_str(), nullptr,
                                             IID_PPV_ARGS(item.Put()))))
        return {};

    return ImageFromShellItem(item.Get(), kSourceIconSize);
}

// Last resort. WM_GETICON is a send, so it can hang on a wedged app; use the
// timeout form and accept losing the icon rather than stalling the worker.
Bitmap ExtractWindowIcon(HWND window) {
    if (!window || !::IsWindow(window)) return {};

    DWORD_PTR result = 0;
    HICON icon = nullptr;

    if (::SendMessageTimeoutW(window, WM_GETICON, ICON_BIG, 0,
                              SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result) && result)
        icon = reinterpret_cast<HICON>(result);

    if (!icon)
        icon = reinterpret_cast<HICON>(::GetClassLongPtrW(window, GCLP_HICON));

    if (!icon) return {};
    return FromHIcon(icon);
}

Bitmap ProduceTile(const Request& request) {
    std::wstring displayName;

    // A user-supplied override wins over anything we can synthesise. Some apps
    // ship an icon that no amount of analysis will make look right, and this is
    // the escape hatch for exactly those.
    Bitmap source = config::LoadThemeOverride(request.exePath, request.aumid);

    if (source.Empty() && request.packaged)
        source = ExtractPackaged(request, displayName);

    if (source.Empty())
        source = ExtractExecutable(request);

    if (source.Empty())
        source = ExtractWindowIcon(request.fallbackWindow);

    if (source.Empty()) {
        MACTAB_WARN("icons: no icon found for %s", ToUtf8(request.key).c_str());
        return {};
    }

    if (!displayName.empty()) {
        // Kept here, under our own lock, rather than pushed into the identity
        // cache: that map is unsynchronised and is mutated (and rehashed) on
        // the UI thread throughout every gesture.
        std::lock_guard<std::mutex> guard(g_lock);
        g_displayNames[request.key] = displayName;
    }

    return MakeIconTile(source, request.size);
}

void Remember(const CacheKey& key, Bitmap tile) {
    std::lock_guard<std::mutex> guard(g_lock);

    g_tiles[key] = std::move(tile);

    g_lru.remove(key);
    g_lru.push_front(key);

    while (g_lru.size() > kMaxCachedTiles) {
        g_tiles.erase(g_lru.back());
        g_inFlight.erase(g_lru.back());
        g_lru.pop_back();
    }
}

void WorkerMain() {
    // The shell interfaces below are apartment-threaded. This thread owns its
    // own STA; the UI thread's apartment is set up separately by the panel.
    ComApartment apartment(COINIT_APARTMENTTHREADED);
    if (!apartment.Ok())
        MACTAB_WARN("icons: worker running without a clean apartment");

    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(g_lock);
            g_wake.wait(lock, [] { return g_stopping || !g_queue.empty(); });
            if (g_stopping && g_queue.empty()) return;

            request = std::move(g_queue.front());
            g_queue.pop_front();
        }

        const CacheKey key{ request.key, request.size };
        const std::wstring cachePath = DiskCachePath(request);

        Bitmap tile;
        if (ReadDiskCache(cachePath, tile) &&
            tile.width == request.size && tile.height == request.size) {
            MACTAB_DIAG("icons: disk cache hit for %s", ToUtf8(request.key).c_str());
        } else {
            const double started = NowMs();
            tile = ProduceTile(request);
            if (!tile.Empty()) {
                MACTAB_DIAG("icons: built tile for %s in %.1f ms",
                            ToUtf8(request.key).c_str(), NowMs() - started);
                WriteDiskCache(cachePath, tile);
            }
        }

        // Record the outcome either way.
        //
        // An app whose icon genuinely cannot be extracted caches an empty tile
        // on purpose: Acquire then reports a cache hit, the panel keeps drawing
        // its placeholder, and the request is not re-queued on every single
        // gesture for the rest of the session. Leaving it unrecorded would pin
        // the in-flight marker forever, which is the same "never retried" result
        // but arrived at by accident and invisible in the log.
        const bool produced = !tile.Empty();
        Remember(key, std::move(tile));

        if (produced) {
            if (g_notifyWindow)
                ::PostMessageW(g_notifyWindow, g_notifyMessage, 0, 0);
        } else {
            MACTAB_WARN("icons: caching empty tile for %s; it will render as a placeholder",
                        ToUtf8(request.key).c_str());
        }
    }
}

} // namespace

bool Start(HWND notifyWindow, UINT notifyMessage) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_worker.joinable()) return true;

    g_notifyWindow  = notifyWindow;
    g_notifyMessage = notifyMessage;
    g_stopping      = false;

    g_worker = std::thread(WorkerMain);
    MACTAB_DIAG("icons: worker started");
    return true;
}

void Stop() {
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!g_worker.joinable()) return;
        g_stopping = true;
        // Abandon anything still queued. Draining it would mean up to a 200ms
        // SendMessageTimeout per pending request, which can add seconds to
        // shutdown, right when Restart Manager is timing us during an upgrade.
        g_queue.clear();
    }
    g_wake.notify_all();

    g_worker.join();
    MACTAB_DIAG("icons: worker stopped");

    std::lock_guard<std::mutex> guard(g_lock);
    g_notifyWindow = nullptr;
}

bool Acquire(const Request& request, Bitmap& out) {
    const CacheKey key{ request.key, request.size };

    {
        std::lock_guard<std::mutex> guard(g_lock);

        const auto found = g_tiles.find(key);
        if (found != g_tiles.end()) {
            out = found->second;

            // Touch for LRU. O(n) on a 96-entry list during a gesture is
            // nothing next to the rest of the frame.
            g_lru.remove(key);
            g_lru.push_front(key);
            return true;
        }

        // Already queued; do not pile up duplicates while the panel redraws.
        if (g_inFlight.find(key) != g_inFlight.end())
            return false;

        g_inFlight[key] = true;
        g_queue.push_back(request);
    }

    g_wake.notify_one();
    return false;
}

std::wstring DisplayName(const std::wstring& key) {
    std::lock_guard<std::mutex> guard(g_lock);
    const auto found = g_displayNames.find(key);
    return (found != g_displayNames.end()) ? found->second : std::wstring{};
}

void ClearMemoryCache() {
    std::lock_guard<std::mutex> guard(g_lock);
    g_tiles.clear();
    g_lru.clear();
    g_inFlight.clear();
}

} // namespace mactab::icons
