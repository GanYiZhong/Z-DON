# Z-DON

English | [繁體中文](README.zh-TW.md)

<img src="docs/logo.png">

**Z-DON** is a fork of [YataiDON](https://github.com/Yonokid/YataiDON), a TJA player and Taiko simulator written in C++.
YataiDON itself is written by its human contributors; **Z-DON's own changes on top of it are developed entirely by AI**,
and focus on aggressive performance work, new gameplay features and online play.

- Website and downloads: <https://zdon.taiko.ac>
- Play in the browser (web version): <https://zdon.taiko.ac/play/>
- License: GPL-3.0 (see [LICENSE](LICENSE)), derived from YataiDON

## Relationship to upstream

- Z-DON keeps merging upstream master, so everything YataiDON has (multi-platform builds, Lua skins, 3D characters,
  AI battle, shop, Dan dojo, tutorial, …) is here too.
- **Compatible**: YataiDON / ESE skins, `scores.db` and `config.toml` work as they are; Z-DON keeps its own data in
  separate tables and settings.
- Most new features can be turned off in `config.toml` or with an environment variable, giving upstream behaviour back.
- The full change log, measurements and switches are in [docs/ZDON_ROADMAP.md](docs/ZDON_ROADMAP.md) (Chinese).

## What Z-DON adds

### Performance

Every item is checked with deterministic tests (fixed time step, bit-exact screenshot comparison) so the picture does
not change, and measured on real hardware.

- **Loading**: cross-screen read-ahead, per-screen texture pack cache (texpack), zlib-ng PNG decoding, BC7 compressed
  texture cache, lazy GPU upload, preparing the next screen in the background (predecode), decoding only the first
  2 s of long sounds up front, walking the song library once. Song select textures from a cold hard disk: about
  1.4 s → 0.35 s.
- **Runtime**: lock-free audio callback, polyphonic hit sounds, a high-resolution timer for the input thread (no longer
  spinning a whole core), vblank-aligned frame pacing, video uploaded as YUV planes and converted in a shader, shared
  glyph cache for text, generational Lua GC, leaner batch drawing, `-O3` + LTO for Windows Release.
- **Texture density**: skins can ship 2× textures and draw them sharply at 4K (`texture_density`).

### Gameplay

- **Pause menu**: Esc / Space pauses during play, with a 3-2-1 countdown before resuming; losing window focus pauses too.
- **Practice / training**: A→B section loops, jump to a measure number, fixed branches, per-hit offset display, fully
  usable with a drum alone.
- **Play options**: constant speed (ignores BPM / SCROLL changes), hidden / sudden, personal-best ghost, sudden death,
  flashlight.
- **Results**: hit timing histogram (mean, standard deviation, early / late counts); the reason is shown when a score
  isn't saved.
- **Song select**: live search panel (genre, difficulty and star filters, mouse support), difficulty folders that list
  only uncleared charts, the last 10 plays of each chart.
- **AI battle** (Z-DON's own): the AI adapts to its level and your timing, scores by sections, and its level follows your
  wins and losses.
- **Recording**: render a chart and difficulty straight to MP4 ("Record" mode in the main menu, or
  `tools/record_video.bat`), several difficulties in one video, parallel segments, a background queue.
- **Chart compatibility**: TJA `COMPAT:` (jiro1 / jiro2), osu!taiko charts straight in the song library, MIDI drum
  mapping learned in the settings screen.
- Many bugs fixed that upstream also has (crash on nested folders, wrong file picked for songs with the same name,
  kana paths that wouldn't open, crash when leaving practice mode, …).

### Online

- **Self-hosted server** [`server/`](server/README.md) (Python standard library only): the game's online API, delta
  update channels, skin publishing.
- **Delta updates**: checked when the game starts; press F9 on the title screen to update (per-file SHA-256; never
  touches settings, scores or songs).
- **Online battle**: room codes or quick match, best of three with players taking turns to pick songs, chat, surrender,
  credit score and reports. Hits go over WebSocket and a direct UDP connection (hole punching) at the same time, both
  sides start on a synchronised clock, and a missing song is sent over automatically.
- **Challenges**: play the same chart against someone's replay.
- **Website** (zdon.taiko.ac): downloads, song and chart pages, rating leaderboard, difficulty tables, player pages
  (nameplate, crown counts, best 50, Dan), and a "my page" (title, nameplate, character look, privacy, score management
  and export).

### Web version

Built with Emscripten to WebAssembly and playable in the browser (<https://zdon.taiko.ac/play/>); audio is mixed in an
AudioWorklet for low latency.

## System requirements

- **Windows**: Windows 10 or later (the main development and test platform)
- **Linux / macOS / Android / iOS / PS5 / Switch**: upstream's builds; Z-DON's changes aim to be cross-platform but are
  tested less
- **Browser**: a recent Chrome / Edge / Firefox with WebAssembly and AudioWorklet

## Installation

1. Download the Windows build from <https://zdon.taiko.ac> and unzip it.
2. Run `YataiDON.exe` (the executable keeps upstream's name to make merging easier).
3. Put TJA songs in `Songs`, or set your song folders in the settings. Later updates are one F9 away in the game.

If latency is high, change `device_type` in the settings to `WDM-KS`, `WASAPI` or `ASIO`.

## Building from source

### Windows (MSYS2 MinGW64)

Install the dependencies as in upstream's [Windows (MSYS2/MinGW64)](<https://github.com/Yonokid/YataiDON/wiki/Windows-(MSYS2-MingW64)>) guide, then:

```bash
export PATH=/c/msys64/mingw64/bin:$PATH   # without this gcc fails silently
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 16
```

The output goes to `build/bin`.

| CMake option | Default | Description |
|---|---|---|
| `ZDON_LTO` | `ON` | Link-time optimization for Windows Release (a full link takes minutes; turn it off while developing, back on for measuring and releases) |
| `ZDON_PCH` | `ON` | Precompiled headers |
| `YATAIDON_PROFILER` | `OFF` | Build in the [Tracy](https://github.com/wolfpld/tracy) v0.14.1 client |
| `YATAIDON_USE_SDL2` | `OFF` | Build against SDL2 instead of SDL3 |

Other platforms follow upstream's wiki: [Linux](https://github.com/Yonokid/YataiDON/wiki/Linux),
[macOS](https://github.com/Yonokid/YataiDON/wiki/Mac-OS), [Android](https://github.com/Yonokid/YataiDON/wiki/Android),
[iOS](platform/ios/README.md), [PS5](platform/ps5/README.md), [Switch](platform/switch/README.md).

### Formatting and linting

Changes under `src/` are formatted with clang-format 23.1.3 (the same as upstream CI):

```bash
find src -type f \( -name '*.cpp' -o -name '*.h' \) -exec clang-format -i {} +
```

clang-tidy, after configuring `build/`:

```bash
run-clang-tidy -quiet -j 4 -p build "^$(pwd)/src/"
```

### Environment variables for development

- `YATAIDON_TEST=1`: muted, hidden window, real input ignored; for automated tests.
- `YATAIDON_FIXED_STEP_MS` + `YATAIDON_DUMP_FRAMES`: fixed time step and screenshots that compare bit for bit.
- `YATAIDON_FRAME_STATS=1`: one line every 2 s with frame times, CPU and memory; `ZDON_FRAME_SPIKE_MS=<ms>` logs
  frames over a threshold.
- `YATAIDON_FAKE_KEYS`, `YATAIDON_JUMP_SCREEN`, `YATAIDON_FORCE_AUTO`: fake key presses, jump straight to a screen,
  force auto play.

The full list, and every performance switch, is in [docs/ZDON_ROADMAP.md](docs/ZDON_ROADMAP.md).

## Controls

- During play, **Esc** / **Space**: pause menu; **F1**: quick restart
- On any screen, **Esc**: back
- Song select: **Space** to favorite, **Left/Right Ctrl** to skip 7 songs, **F2** to search, **F5** to refresh
- Online battle menu: **Y** to chat, **R** to report
- Title screen, **F9**: update to the latest version
- Drum keys can be changed in the in-game settings or in `config.toml`

## Reporting issues

Please report Z-DON problems in this repository's [Issues](https://github.com/GanYiZhong/Z-DON/issues), not upstream;
problems with YataiDON itself belong in [YataiDON's Issues](https://github.com/Yonokid/YataiDON/issues).

## Acknowledgments

Z-DON is built on the work of [YataiDON](https://github.com/Yonokid/YataiDON) and all its contributors:
IID, mc08, QBaraki, KabanFriends, DragonRatTiger, Komi, sumandrew, WallK, dpkgluci, Ape, chuchy (osu parser),
churrochef (vertical scroll), cainan (3D don chan), RyutoSetsujin, Nyannurs, fehpizza (logo), Radix, Thonktheta,
SomehowScarlet, SunnyDeez. The website's difficulty table data comes from
[YataiDON-Syakai](https://github.com/Yonokid/YataiDON-Syakai).

Libraries: [raylib](https://www.raylib.com/), [SDL3](https://github.com/libsdl-org/SDL), [SQLite](https://www.sqlite.org/),
[RapidJSON](https://github.com/Tencent/rapidjson), [toml++](https://github.com/marzer/tomlplusplus), [spdlog](https://github.com/gabime/spdlog),
[Lua](https://github.com/marovira/lua), [sol2](https://github.com/ThePhD/sol2), [cpptrace](https://github.com/jeremy-rifkin/cpptrace),
[libsndfile](https://github.com/libsndfile/libsndfile), [libsamplerate](https://github.com/libsndfile/libsamplerate),
[FFmpeg](https://ffmpeg.org/), [PortAudio](http://www.portaudio.com/), [miniz](https://github.com/richgel999/miniz),
[zlib-ng](https://github.com/zlib-ng/zlib-ng), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), [curl](https://curl.se/).
