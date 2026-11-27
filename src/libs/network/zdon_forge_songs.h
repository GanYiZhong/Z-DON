#pragma once

#include <atomic>
#include <filesystem>
#include <string>

// Z-DON: the logged-in TaikoForge member's own charts (forge.taiko.ac) as a song folder, as the web version has them
// (Z-DON-Web platform/web/shell.html). The server relays TaikoForge's partner API for the game's login token
// (server/zdon_game_login.py: GET /forge_songs, GET /forge_song?id&file=tja|audio). The charts go into
// <the first song folder>/TaikoForge/<title>/song.tja with the audio beside it as song.ogg; each chart folder has a
// .forge_id naming its TaikoForge chart, so a chart deleted there is removed here and nothing else is ever touched,
// and a .forge_stamp (the version the list names): a chart already here as TaikoForge has it is not asked again.
namespace zdon_forge_songs {

    struct Progress {
        std::atomic<int> done{0};
        std::atomic<int> total{0};
    };

    // the TaikoForge folder (it may not exist yet)
    std::filesystem::path folder();

    // Brings the folder up to date for this login token: the list and every chart's TJA, audio not yet here.
    // Blocks (run it on a worker). True when anything on disk changed (the song list should be walked again);
    // false also when the server could not be asked (nothing is removed then).
    bool sync(const std::string& token, Progress& progress);

} // namespace zdon_forge_songs
