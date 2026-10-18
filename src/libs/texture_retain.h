#pragma once

#include <memory>
#include <string>

#include "ray.h" // IWYU pragma: keep

struct TextureObject;

// Z-DON: keeps the textures of the "hub" screens (song select, dan select) loaded after the
// screen ends, so coming back to it from a song (the most frequent transition in the game) does
// not read, decode and upload a few hundred PNGs again. load_folder hands every texture object
// it creates for such a screen to add(), and asks reuse() before loading one.
//
// Bounded by a GPU memory budget (bytes of the retained textures, counted once per GPU texture):
//   YATAIDON_RETAIN_MB=<n> sets it, 0 turns retention off. Default: a quarter of the dedicated
//   video memory the driver reports (NVIDIA / AMD extensions), clamped to 256..1024 MB; 512 MB
//   when the driver does not say; off on Android, iOS and the web.
// Over budget, the least recently used screen's set is dropped first; whatever still doesn't
// fit is simply not retained (it loads from disk next time, as before).
// clear() (skin load/unload) drops everything.
namespace texture_retain {

    // Whether textures load_folder creates for screen_name are retained.
    bool wanted(const std::string& screen_name);

    // A texture object load_folder just created for screen_name, in the state its own folder's
    // texture.json gave it; key is the load_folder cache key (folder / name).
    void add(const std::string& screen_name, const std::string& key, const std::shared_ptr<TextureObject>& obj);

    // The retained object for key; nullptr when not retained. Unless something else still holds
    // it, it is put back to the state add() saw first (anything a screen or the debug menu changed
    // afterwards is undone, as a fresh load would).
    std::shared_ptr<TextureObject> reuse(const std::string& key);

    bool contains(const std::string& key);

    // Every texture screen_name loaded last time is retained (the screen's Graphics folder need
    // not be read ahead).
    bool screen_complete(const std::string& screen_name);

    // Logs what screen_name holds and how many objects were reused for it since the last call.
    void log_screen(const std::string& screen_name);

    void clear();

    // Frees a decoded image on a background thread (the frees of a screen's images took tens of
    // ms on the main thread). Frees in place when too much is already waiting.
    void free_image_async(ray::Image& image);

} // namespace texture_retain
