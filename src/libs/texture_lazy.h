#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gfx.h" // IWYU pragma: keep
#include "raylib_ns.h"

// Z-DON: skin textures are uploaded to the GPU on first use instead of when their screen loads.
// Song select alone loads ~525 textures (~456 MB of RGBA) of which only a fraction is drawn on entry
// (one genre background of ten, the crown art, the difficulty select...); uploading all of them made
// the entry frame and the next one (the driver's deferred copies) take ~550 + ~380 ms.
//
// load_folder hands the decoded image to make(): the returned texture has id 0 and the image's width,
// height, mipmaps and format, and keeps the image. Whatever needs the GL texture goes through
// TextureObject::frame_texture(), which calls upload() for a texture still pending: same frame, so a
// texture can never show blank. Filter and wrap set before the upload (set_filter / set_wrap) are
// applied right after it, in the eager order. Freed unused, the image is simply dropped.
// The folders drawn while a song plays (global, game, background, and anything loaded during a song)
// still upload at load: a first-use upload there would be a dropped frame mid-song.
//
// ZDON_LAZY_UPLOAD=0 uploads everything at load as before (default on; off on Android, iOS and the
// web). ZDON_LAZY_UPLOAD_MB=<n> caps the decoded images waiting in RAM (default 768); past it,
// textures upload at load again (logged once per screen).
//
// Eviction: a texture still pending when its screen ends would keep its decoded image in RAM while the
// other screens run (song select alone ~400 MB, retained or held by its Lua scripts). At every screen
// change evict_all() frees those images; the texture stays pending, with the file it came from. The
// next screen load that holds such a texture (in its texture map or on-demand stash) decodes it again
// with its own textures (texture pack, parallel), and one drawn while evicted decodes on the spot
// (logged as a late decode). ZDON_LAZY_EVICT=0 keeps the images as before; ZDON_LAZY_KEEP_MB=<n> keeps
// up to n MB of them (the smallest first), default 0.
//
// Prefill: on the result screens, the screen that most likely comes next (result -> song select,
// dan result -> dan select) gets its evicted images decoded again on a few background jobs
// (texture.cpp, TextureWrapper::start_prefill). They wait there and only go to the textures when that
// screen loads (an eviction in between would free them again); a load of any other screen drops them.
// ZDON_LAZY_PREFILL=0 turns it off, =<n> uses n jobs (default 4); ZDON_LAZY_PREFILL_SLOW_MS=<ms> (dev) waits
// that long before each image.
//
// Warm-up: a texture first drawn in the middle of a screen (song select's difficulty select: 12 textures,
// 12 ms in one frame; a crown 16 ms in another) still uploads in the frame that draws it. Every first-use
// upload is recorded as (screen whose load made it, file), in cache/warm_upload.txt (at most 512 per screen,
// written at a screen change when it changed). After a screen loads, warm_tick() uploads the recorded
// textures it holds still pending, in the order they were first drawn, a 1 MB band of rows per call
// (glTexSubImage2D) until ZDON_WARM_UPLOAD_MS of the frame (default 1.0) is spent; it runs after the
// frame's present, in the time the frame limiter would sleep. Each texture is drawn once into a 1x1 target
// right after it is allocated and once when complete, so the driver moves it into video memory while
// empty instead of all at once on its first real draw. A texture keeps id 0 until all its rows
// are in, so drawn earlier it finishes its upload on the spot as before: no frame shows anything else.
// An unfinished one is dropped at the screen change. ZDON_WARM_UPLOAD_MS=0 turns it off (no recording).
// ZDON_LAZY_UPLOAD_LOG=1 (dev) logs each texture uploaded on its first use.
namespace texture_lazy {

    bool enabled();

    // A texture that uploads `image` on first use, or nullptr when lazy upload is off, over the cap or the
    // folder is one drawn during play
    // (the caller uploads eagerly then, and reports it with count_eager). On success the image belongs to
    // the texture: the caller must not free it (it may keep reading it until it forgets the struct).
    // source: the image's file (to decode it again after an eviction); owner: the screen whose load made it
    // size / mtime: the source as listed (the texture pack checks them when it is decoded again)
    std::shared_ptr<Texture> make(const Image& image, const std::string& screen, const std::filesystem::path& source,
                                  const std::string& owner, uint64_t size = 0, uint64_t mtime = 0);

    // an eager upload at load, for the per-screen log
    void count_eager(const std::string& screen, const Texture& t);

    // Uploads t if it is still pending (id 0); no-op otherwise.
    void upload(const Texture* t);

    // SetTextureFilter / SetTextureWrap, remembered until the upload for a pending texture
    void set_filter(const Texture* t, int filter);
    void set_wrap(const Texture* t, int wrap);

    // Screen change: frees the decoded images of all pending textures (see above).
    void evict_all();
    bool evicted(const Texture* t);
    // the file an evicted texture decodes from
    std::filesystem::path source(const Texture* t);
    // the files of the evicted textures a load of `owner` made
    std::vector<std::filesystem::path> evicted_sources(const std::string& owner);
    // gives an evicted texture its decoded image back (the texture owns it from then on; freed when no
    // longer evicted or when image.data is null)
    void refill(const Texture* t, Image image);
    // the evicted files of owner, each once (sorted by path), and the RAM their images take
    struct EvictedFile {
        std::filesystem::path source;
        uint64_t size = 0; // as listed when the texture was made
        uint64_t mtime = 0;
        size_t bytes = 0; // decoded
    };
    std::vector<EvictedFile> evicted_files(const std::string& owner);
    // gives each image to the evicted textures of owner made from its file (a copy for the second and
    // later ones); images nothing takes are freed. Returns the textures refilled.
    size_t refill_from(const std::string& owner, std::vector<std::pair<std::filesystem::path, Image>>& images);
    // how many more bytes of images may wait in RAM under the cap (ZDON_LAZY_UPLOAD_MB)
    size_t room_bytes();
    // ZDON_LAZY_PREFILL: 0 = off, else the number of background jobs
    unsigned prefill_jobs();
    // how to decode a source when an evicted texture is drawn
    void set_decoder(Image (*decode)(const std::filesystem::path& source));

    // Logs the pending textures uploaded since the last call (all screens), then what screen_name's
    // load uploaded at once vs left pending; queues the warm-up of screen_name.
    void log_screen(const std::string& screen_name);

    // Once per frame, after its present: uploads queued warm-up textures for up to ZDON_WARM_UPLOAD_MS.
    void warm_tick();

    // Z-DON: the files an earlier load of owner drew (cache/warm_upload.txt), as generic paths; main thread, a copy
    std::unordered_set<std::string> warm_sources(const std::string& owner);
    // the largest image a game / global / background texture no earlier load drew may wait for its first draw
    // with (ZDON_LAZY_GAME_MAX_KB, default 1 MB); 0 with ZDON_LAZY_GAMEPLAY=0
    size_t gameplay_lazy_max_bytes();

    // Z-DON predecode (TextureWrapper::start_predecode): an image uploaded the warm-up's way, a 1 MB band of rows
    // per call into a texture drawn once into a 1x1 target when allocated and once when complete. True once every
    // row is in (st.id is the texture then); false with st.id 0 when it cannot be (not bandable, allocation failed).
    struct BandUpload {
        unsigned int id = 0;
        int rows_done = 0;
    };
    bool bandable(const Image& image);
    bool upload_band(const Image& image, BandUpload& st);

} // namespace texture_lazy
