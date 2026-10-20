#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ray.h" // IWYU pragma: keep

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
namespace texture_lazy {

    bool enabled();

    // A texture that uploads `image` on first use, or nullptr when lazy upload is off, over the cap or the
    // folder is one drawn during play
    // (the caller uploads eagerly then, and reports it with count_eager). On success the image belongs to
    // the texture: the caller must not free it (it may keep reading it until it forgets the struct).
    // source: the image's file (to decode it again after an eviction); owner: the screen whose load made it
    std::shared_ptr<ray::Texture2D> make(const ray::Image& image, const std::string& screen,
                                         const std::filesystem::path& source, const std::string& owner);

    // an eager upload at load, for the per-screen log
    void count_eager(const std::string& screen, const ray::Texture2D& t);

    // Uploads t if it is still pending (id 0); no-op otherwise.
    void upload(const ray::Texture2D* t);

    // SetTextureFilter / SetTextureWrap, remembered until the upload for a pending texture
    void set_filter(const ray::Texture2D* t, int filter);
    void set_wrap(const ray::Texture2D* t, int wrap);

    // Screen change: frees the decoded images of all pending textures (see above).
    void evict_all();
    bool evicted(const ray::Texture2D* t);
    // the file an evicted texture decodes from
    std::filesystem::path source(const ray::Texture2D* t);
    // the files of the evicted textures a load of `owner` made
    std::vector<std::filesystem::path> evicted_sources(const std::string& owner);
    // gives an evicted texture its decoded image back (the texture owns it from then on; freed when no
    // longer evicted or when image.data is null)
    void refill(const ray::Texture2D* t, ray::Image image);
    // how to decode a source when an evicted texture is drawn
    void set_decoder(ray::Image (*decode)(const std::filesystem::path& source));

    // Logs the pending textures uploaded since the last call (all screens), then what screen_name's
    // load uploaded at once vs left pending.
    void log_screen(const std::string& screen_name);

} // namespace texture_lazy
