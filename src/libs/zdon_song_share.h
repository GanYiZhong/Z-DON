#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

// Z-DON live battle: a guest without the host's chart gets it from the host's game (through the server).
// The host zips the chart's folder; the guest unpacks it under "<first song folder>/Z-DON Online/<name>",
// checks that the course it was sent for has the host's hash, and adds it to its library.

constexpr size_t kZdonSongShareMax = 85u * 1024u * 1024u; // the zip (the server takes up to 90 MB)

struct ZdonSongPack {
    std::string zip;   // empty: could not be packed (error says why)
    std::string name;  // the folder name to unpack into
    std::string error; // "not_tja" / "too_big" / "read"
};

// the chart's folder (every file when it holds this chart only; else the chart and its audio / movie)
ZdonSongPack zdon_pack_song(const fs::path& chart);

struct ZdonSongInstall {
    std::optional<fs::path> chart; // the installed chart (registered with the library), when its hash matched
    std::string error;             // "extract" / "no_chart" / "hash"
};

// unpacks `zip` and checks the chart against `want_hash` (one of its courses' hashes); a mismatch is removed again
ZdonSongInstall zdon_install_song(const std::string& zip, const std::string& name, const std::string& want_hash);
