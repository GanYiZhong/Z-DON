#pragma once

#include <string>
#include <vector>

// Z-DON: learned prefetch of the texture folders a skin's Lua loads on demand. tex.get_texture of a
// texture from another screen's folder (e.g. song select's dan transition drawing
// "dan_select/global/bg") makes resolve_lua_texture load that folder synchronously on the main thread:
// a 1080p PNG read, decoded and uploaded mid-animation (measured 32.5 ms). Each such load is recorded
// as (screen showing -> folder), and the next time that screen loads, load_screen_textures decodes the
// recorded folders in the same batch (parallel decode, texture pack, lazy upload) into a stash, not
// into `textures`: the script's request still installs the folder at the same moment (it may replace
// textures the screen has), now without disk or decode. The stash goes with unload_textures, as an
// on-demand folder does.
//
// cache/ondemand/<skin>.txt: one "<screen>\t<folder screen>/<subset>" line per pair, at most
// kMaxPerScreen per screen (the oldest go first). Written when it changed, from unload_textures (once
// per screen change). A recorded folder that is gone is dropped from it.
// ZDON_PREFETCH_ONDEMAND=0 turns recording and prefetching off (the file is left as it is).
namespace texture_ondemand {

    constexpr size_t kMaxPerScreen = 64;

    struct Folder {
        std::string screen; // the folder's screen ("dan_select")
        std::string subset; // its subset under that screen ("global")
    };

    bool enabled();

    // folder_screen/subset was loaded on demand while `screen` was showing, with skin `skin`
    void record(const std::string& skin, const std::string& screen, const std::string& folder_screen,
                const std::string& subset);

    // the folders recorded for `screen`, oldest first
    std::vector<Folder> folders(const std::string& skin, const std::string& screen);

    // drops a recorded folder (it no longer exists)
    void forget(const std::string& skin, const std::string& screen, const Folder& folder);

    // writes the files of the skins whose records changed
    void flush();

} // namespace texture_ondemand
