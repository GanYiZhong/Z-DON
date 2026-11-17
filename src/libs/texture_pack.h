#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gfx.h" // IWYU pragma: keep

// Z-DON: a derived cache of each screen's texture files, so a screen reads one file sequentially
// instead of a few hundred small PNGs (on a hard drive those are random reads: about a second for
// song select when nothing is cached). The skin itself is unchanged: the pack only holds copies of
// the PNG bytes load_screen_textures resolved, and the loose files stay the source of truth.
//
// cache/texpack/<skin>/<screen>.pack: header, index (path as load_screen_textures plans it, size and
// last-write time from the folder listing the load made anyway, offset, length, hash), payloads in
// the order the screen loads them. An entry is used only when the listing still shows the same
// size and time and its bytes hash right; anything else (edited or replaced file, child skin now
// overriding it, truncated or corrupt pack) reads the loose file, and the pack is rewritten in the
// background from the bytes that load read (one file, written to .tmp and renamed).
// The cache is bounded (YATAIDON_TEXPACK_MB, default 1024): the least recently written packs go.
// YATAIDON_TEXPACK=0 turns it off.
namespace texture_pack {

    bool enabled();

    // cache/texpack/<skin>/<screen>.pack
    std::filesystem::path pack_path(const std::filesystem::path& skin, const std::string& screen);

    struct FileStat {
        uint64_t size = 0;
        uint64_t mtime = 0;
    };

    // One screen load's use of its pack. Thread-safe: the image prefetch workers call get/put.
    class Session {
    public:
        struct Planned {
            std::string key; // ImagePrefetch::image_key of the file
            FileStat stat;   // from the load's folder listing
        };
        // For an entry the load did not plan: 1 = listed with this stat, 0 = its folder is listed and it
        // is gone, -1 = its folder was not listed (kept as it is).
        using Peek = int (*)(void* ctx, const std::string& key, FileStat& out);

        // background: the pack is read at low CPU and I/O priority (a prefill nobody waits for yet)
        Session(std::filesystem::path pack, std::string screen, std::vector<Planned> planned, Peek peek, void* peek_ctx,
                bool background = false);
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // The pack's copy of key's file when it is current (waits while it is still being read).
        bool get(const std::string& key, const uint8_t*& data, size_t& size);
        // The pack's copy did not decode: don't trust it again.
        void reject(const std::string& key);
        // key's file has to be read from the skin, and its bytes would go into the rewritten pack
        bool wants(const std::string& key);
        // bytes of key's file as just read from the skin
        void put(const std::string& key, std::vector<uint8_t> bytes);

        // The load is done (no more get/put): queue the rewrite when the pack missed anything.
        void finish();

        // Z-DON: every file the pack holds (key and stat as written), planned or not; empty without a pack
        std::vector<Planned> listed();

    private:
        struct Span;
        struct Entry {
            std::string key;
            FileStat stat;
            uint64_t offset = 0; // in the old pack's payload area
            uint64_t size = 0;
            uint64_t hash = 0;
            int state = 0; // 0 none, 1 hit (in a span), 2 checked ok, 3 bad, 4 hit left unread (BC7 cache has it)
            std::shared_ptr<Span> span;
            size_t span_offset = 0;
            std::shared_ptr<std::vector<uint8_t>> fresh; // read from the skin this time
            bool planned = false;
            bool carry = false; // not planned, still current: copied over from the old pack
        };

        void read_index();
        void read_spans();

        std::filesystem::path path;
        std::string screen;
        std::vector<Entry> entries; // planned first (in plan order), then old-pack entries not planned
        std::unordered_map<std::string, size_t> by_key;
        std::vector<std::shared_ptr<Span>> spans;
        uint64_t payload_start = 0;
        bool pack_ok = false;
        size_t hits = 0;
        size_t fresh_count = 0;
        size_t bc7_skipped = 0;
        uint64_t fresh_bytes = 0;
        bool dirty = false; // something stale or rejected

        std::mutex mutex;
        std::condition_variable cv;
        std::thread reader;
        bool stop = false;
    };

    // Reads path into bytes; false when it can't be read.
    bool read_file(const std::filesystem::path& path, std::vector<uint8_t>& bytes);

} // namespace texture_pack
