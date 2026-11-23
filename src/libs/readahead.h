#pragma once

#include <filesystem>
#include <string>

// Warms the OS file cache with the screens likely to come next, so their textures and sounds
// come from memory instead of the disk when the player gets there. On a hard drive a screen's
// few hundred small PNGs take seconds to read cold; reading them while the player is still
// on the previous screen hides that. Runs on one background thread at low CPU and I/O
// priority (POSIX: posix_fadvise WILLNEED), holds no data itself, and a newer request
// replaces an unfinished one.
namespace zdon_readahead { // not "readahead": glibc declares a readahead() function

    // Called once a screen has initialized.
    void after_screen(const std::string& screen_name);

    // Z-DON: a song was picked in song select. Reads its folder (the chart, the audio, a video) first,
    // then what after_screen("song_select") reads (the game and result screens).
    void prepare_song(const std::filesystem::path& chart);

    // Drops the current request (a screen starts loading: its own reads go first).
    void cancel();

} // namespace zdon_readahead
