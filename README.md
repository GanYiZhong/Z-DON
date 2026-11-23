# Z-DON

English | [繁體中文](README.zh-TW.md)

<img src="docs/logo.png">

**Z-DON** is a fork of [YataiDON](https://github.com/Yonokid/YataiDON), a TJA player and Taiko simulator written in C++.
YataiDON itself is written by its human contributors; **Z-DON's own changes on top of it are developed entirely by AI**,
and focus on aggressive performance work, new gameplay features and online play.

- Website: <https://zdon.taiko.ac>
- Download for Windows: [Z-DON-windows.zip](https://github.com/GanYiZhong/Z-DON/releases/latest/download/Z-DON-windows.zip) (rolling release: always the latest build; Linux, macOS and Android builds are in the same [release](https://github.com/GanYiZhong/Z-DON/releases/tag/rolling))
- Play in the browser (web version): <https://zdon.taiko.ac/play/>
- License: GPL-3.0 (see [LICENSE](LICENSE)), derived from YataiDON

## Contents

- [Relationship to upstream](#relationship-to-upstream)
- [Z-DON and YataiDON at a glance](#z-don-and-yataidon-at-a-glance)
- [What Z-DON adds](#what-z-don-adds): [performance](#performance), [graphics and skins](#graphics-and-skins),
  [gameplay](#gameplay-and-play-options), [practice](#practice), [song select](#song-select), [results](#results),
  [AI battle](#ai-battle), [recording](#recording), [charts and input](#charts-and-input-devices), [online](#online),
  [web version](#web-version)
- [Where Z-DON does things differently from upstream](#where-z-don-does-things-differently-from-upstream)
- [Bugs fixed](#bugs-fixed) · [System requirements](#system-requirements) · [Installation](#installation) ·
  [Settings](#settings) · [Building from source](#building-from-source) · [Controls](#controls)

## Relationship to upstream

- Z-DON keeps merging upstream master (currently synced to `dd478af2`, 2026-11-23), so everything YataiDON has
  (multi-platform builds, Lua skins, 3D characters, AI battle, enso battle, shop, events, Dan dojo, tutorial, touch input, online song downloads, …) is
  here too.
- **Compatible**: YataiDON / ESE skins, `scores.db` and `config.toml` work as they are. Z-DON keeps its own data in
  separate tables (`zdon_player_options`, `zdon_ghost`, `zdon_ai_sections`, `zdon_ai_section_best`, `zdon_outfits`)
  and in `zdon_*` settings, so going back to upstream loses nothing.
- New features are either off by default or can be turned off (a `config.toml` key or an environment variable), giving
  upstream behaviour back. See [Settings](#settings).
- The executable keeps upstream's name (`YataiDON.exe`) to keep merges simple.
- The full change log, every measurement and every switch are in [docs/ZDON_ROADMAP.md](docs/ZDON_ROADMAP.md) (Chinese).

## Z-DON and YataiDON at a glance

| | YataiDON | Z-DON |
|---|---|---|
| Song select textures, cold hard disk | about 1.4 s | about 0.35 s |
| Memory while playing (private / working set) | about 680 / 480 MB | about 455 / 305 MB |
| Skin textures in VRAM | uncompressed RGBA | BC7-compressed cache (SimpleStyle song select 538 → 266 MB) |
| Input thread | spins a whole core | high-resolution timer, about 1 ms polling |
| Frame pacing | sleep-based | vblank-aligned, tear-free |
| Windows download | about 91 MB | about 41 MB (decode-only FFmpeg, stripped exe) |
| Updates | download a new release | in game: F9 on the title screen, per-file delta |
| Pause during play | upstream pause menu | Z-DON pause menu with 3-2-1 countdown, pauses on focus loss |
| Practice | basic | A–B loops, bar jumps, fixed branches, per-hit offsets, metronome |
| Play options | upstream set | plus equal speed, hidden / sudden, PB ghost, sudden death, flashlight |
| Online | upstream online features | plus own server, online battle (rooms / quick match), challenges, website |
| Recording to video | — | chart + difficulty straight to MP4, several lanes in one video |
| 3D view of the lane | — | optional (`zdon_lane3d`) |
| Charts | TJA, osu (upstream parser) | plus TJA `COMPAT:`, OpenTaiko note letters, `#LEVELHOLD`, osu!taiko in the library |
| Input devices | keyboard, controllers | plus MIDI drums (learn mapping), USIO arcade I/O (experimental) |

All numbers were measured on the development machine; details are in the roadmap.

## What Z-DON adds

### Performance

Every change is checked with deterministic tests (fixed time step, bit-exact screenshot comparison of 121–349 shots),
so the picture does not change, and measured on real hardware.

**Loading**

- **Read-ahead across screens**: the files the next screen needs are read into the OS cache in parallel (the hard disk
  reorders the requests) and cancelled when another screen starts loading. Song select textures from a cold hard disk:
  1.4 s → 0.35 s.
- **Texture pack cache** (`cache/texpack/`): each screen's images are read from one pack instead of many small files;
  PNGs are decoded with zlib-ng (1.75× faster). Packs leave out files the BC7 cache already serves.
- **BC7 texture cache** (`cache/bc7/`, zstd): skin images are compressed once, checked against a quality gate (every
  pixel within 4/255 or PSNR ≥ 45 dB, otherwise the image stays RGBA) and later loaded without decoding the PNG.
  Needs OpenGL 4.2 / BPTC.
- **Lazy GPU upload**: a texture is uploaded the first time it is drawn; textures earlier visits drew are uploaded
  ahead of time in 1 MB slices after each frame (`cache/warm_upload.txt`), so the first frame does not stall.
  Never-drawn images are released at screen change and refilled in the background when needed again.
- **Next screen prepared in the background (predecode)**: opening difficulty select prepares the play screen, the last
  note prepares the result screen, the title prepares song select, and so on. Song select → game: 754 → 454–717 ms;
  game → result: 159 → 83 ms.
- **Song library walked once** (LibraryIndex), in parallel with skin loading; song boxes come from cached records, so
  charts are read only when needed. Folder scan: 1.7–2.0 s → 0.23 s.
- **Sounds**: long sounds decode their first 2 s up front and the rest in the background (song select BGM 211 → 23 ms);
  menu and result sounds load on first use; combo voices load only for the side that plays and the combo counts the
  chart can reach.
- **Only what the play needs**: AI battle, dan, tutorial and enso battle folders load only in their own modes; the
  `gogo_time`, `kusudama` and `balloon` folders wait until the chart actually has GOGO, kusudama or balloon notes.
- **Kept across screens**: song select and dan select textures stay resident through play and results (result → song
  select textures 234 → 32 ms); hit sounds and global sounds stay loaded.
- **3D Don**: animations are pre-parsed in the background (start → entry ready 765–829 → 386–393 ms) and character
  parts are reused across screens.
- **Faster TJA parser**: a command table instead of regex, string views; parsing the whole library 1.0–1.1 s → 0.64–0.70 s.

**Runtime**

- **Input thread on a high-resolution timer**: no longer spins a whole CPU core, still polls about every 1 ms.
- **vblank-aligned, tear-free frame pacing**, also with vsync on (main thread 95.6% → 20.8% of a core with vsync).
- **Lock-free audio callback**: streams decode on their own thread; worst callback 10.6 → 0.3 ms.
- **Polyphonic hit sounds**: 6 voices each for don and ka, so fast rolls don't cut each other off.
- **Video uploaded as YUV planes** and converted in a shader (decode thread 12.2 → 7.8%); BT.709 videos now use the
  right colour matrix.
- **Text**: a shared glyph cache with an incremental atlas (0.64 → 0.11 ms per text).
- **Lua**: generational GC, a full GC at each screen change, method caches; worst frame on a drumroll chart 5.4 → 2.6 ms.
- **Drawing**: leaner note drawing, deferred blend switches, fewer batch flushes, a raylib patch that skips unused
  normals (GL driver time about −7%).
- **Auto play after a stall**: notes and drumrolls skipped during a frame stall are replayed (also for the AI lane).
- **Build**: Windows Release uses `-O3` + LTO (about −14% CPU per frame) and the Windows segment heap.

**Memory**

- 16-bit PCM for sounds (differences at most −99 dBFS), BC7 images not kept in RAM, textures that would be thrown away
  right after loading are never read, large sprite atlases trimmed of empty borders (Nijiiro explosion 34 → 18 MB VRAM),
  memory-mapped fonts, smaller chart parser structures.
- Total while playing: private about 680 → 455 MB (−33%), working set about 480 → 305 MB (−37%). Title screen
  committed memory 531 → 364 MB.

**Download size**

- Decode-only FFmpeg (DLLs 150 → 24 MB), stripped executable (33.8 → 23.2 MB; symbols kept on the server, crash logs
  print module + address), gzip for large update files (exe update 23.3 → 9.7 MB). Windows zip 91 → 41 MB.
- Linux and macOS builds use a static decode-only FFmpeg too (Linux 178 → 57 MB).

### Graphics and skins

- **Texture density**: a skin can ship 2×–4× art (`screen.texture_density`, or `"density"` per texture) and the frame
  is rendered at that resolution, so it stays sharp at 4K.
- **BC7 compressed textures** (`[video] zdon_texture_compression`, on by default).
- **2D outfits** for skins without 3D characters (`zdon_outfits` in `skin_config`), chosen on the entry screen.
- **Engine-drawn panels** (practice info, timing histogram, ghost, timing bar, "new best") work in any skin, with
  built-in Japanese / English / Chinese / Korean text that a skin can override and reposition.
- **More Lua API for skins**: last hit, hit offsets, song progress, best score, recent plays, chart statistics,
  practice loop, AI battle state, coin credits, clickable areas, screen desaturation, additive blend, and more.
- **Fonts and languages**: `screen.font_base` (language fonts as overlays on a base font), an AUTO language that follows
  the system, Traditional Chinese uses the skin's `font_tw`, `<name>_<lang>` texture variants
  (`language_variants`).
- **Skin chain**: a missing parent skin ends the chain instead of failing; the skin's own fonts come before its parents'.
- **Bundled skins**: the Windows zip ships YataiDONSimpleStyle and SimpleStyleDark; their parents (Nijiiro,
  PyTaikoGreen) are downloaded on first run, from one archive (about 3¾ min instead of 11 min).
- **Note spacing** follows the arcade (the lane scrolls as if 82 px wider at 720p).
- **Clearer failures**: the log names the GPU, and an OpenGL older than 3.3 shows a message instead of a black window.
- **Composited borderless fullscreen** on Windows (`zdon_composited_fullscreen`, off by default).

### Gameplay and play options

- **Pause menu** (`pause_menu`, on by default): Esc or Space during play; resume, restart or choose another song, with a
  3-2-1 countdown before resuming; losing window focus pauses too; drum presses during the pause are ignored. A paused
  play is not saved (same as upstream). Skins with `pause_menu.lua` draw it themselves.
- **Play options**, stored per player in Z-DON's own table:
  - **Equal speed**: every note at the chart's base BPM, ignoring BPM / SCROLL changes (not saved as a score).
  - **Hidden / Sudden**: 25 / 50 / 75%.
  - **Personal-best ghost**: "PB ±N" pace display during play.
  - **Sudden death**: ends at the first bad, or at the first ok (not saved).
  - **Flashlight**.
- **Play aids** (settings, all off by default): timing bar under the judge circle, target score chip, "New best +N"
  on results, results timing panel open by default, a white cross on ka notes (colour-blind aid), remember difficulty
  per song, random / uncleared song keys.
- **Hit timing options** (off by default): play the hit sound from the input thread, judge at key-press time, decide
  branches one measure early.
- **3D view** (`zdon_lane3d`, off by default): the play screen in 3D with coin-shaped notes, lighting and shadows;
  drag / wheel / middle click to move the camera, four presets. Works in normal play, 2P, waiwai, dan and practice.
- **Drum-only menus**: settings can be changed with the drum alone (key capture, duplicate-key protection, "reset to
  default" rows, hold to repeat).
- **Mouse in menus**: left click confirms, right click goes back, the wheel moves.
- **In-game IME**: text input candidates are drawn in the game (fullscreen hid the system window).
- **Coin mode** (`zdon_coin_mode`, off by default) for cabinet-style credits.

### Practice

Modelled on PaulSimulator's training mode; usable with a drum alone.

- **A–B section loop** (`zdon_practice_loop`, on by default): notes outside the loop are greyed, 2 s lead-in, music fades
  out at the loop end, recent passes and the best pass are listed.
- **While paused**: type a bar number and Enter to jump, F6 (or both dons) to fix the branch, ka steps 4 / 8 bars,
  M jumps to the last miss, per-hit offsets above the notes.
- BPM / branch / bar info row, optional metronome, 3D view.

### Song select

- **Search panel** (F2): live search with genre (Tab), difficulty (F3) and star (PgUp / PgDn) filters, mouse support;
  Enter jumps to the song.
- **Difficulty folders list only uncleared charts** (`zdon_sort_filter`, on by default; off = upstream order).
- **Recent plays**: the last 10 plays of each chart.
- Options are saved when the song starts (the select timer no longer starts a song with stale options), and a don
  pressed while the list is still moving waits for it to settle.

### Results

- **Hit timing histogram** (press ka): mean, standard deviation, early / late counts, and the mean of each quarter of
  the song.
- **The reason a score was not saved** is shown (auto play, replay, equal speed, sudden death, recording).

### AI battle

Z-DON has its own AI battle following the arcade rules, next to upstream's.

- Entry screen "AI battle" → AI song select → a 2P play against the AI.
- The AI is tuned by star level and a 1–5 condition and follows your timing; the song is cut into sections at bar
  lines and each section goes to whoever wins it; ties go to the player.
- Best crown and record per section are saved, with rank, rewards, sound cues, a detailed result and a summary of all
  sections.
- Which one runs: if the skin has upstream's `ai_support_menu` Lua script, upstream's AI battle is used; otherwise
  Z-DON's.

### Recording

- Render a chart and difficulty straight to MP4: "Record" mode in the main menu (several courses at once, a background
  queue with remaining time), or `tools/record_video.bat` (drag a `.tja` onto it), or the command line:
  `YataiDON.exe <chart.tja> [difficulty] --record out.mp4 [--auto] [--record-fps 60]`.
- Several lanes in one video (`--record-tracks 4,3,2,1,0`: 1P + 2P, or up to 5 stacked lanes over a blurred
  background), clips (`--record-clip`), per-lane volume.
- Fast: GPU colour conversion and three segments in parallel (a 239 s video in about 45 s; a 5-lane 179 s video in
  36.5 s). Needs `ffmpeg` on PATH (or `ZDON_FFMPEG`). Cancelled recordings leave no part files behind.

### Charts and input devices

- **TJA**: `COMPAT:` jiro1 / jiro2, OpenTaiko note letters (A / B big don / ka, D balloon), `#LEVELHOLD`, score
  branch conditions, per-branch balloon counts, P1 / P2 charts end at their own `#END`, STYLE sections keep their own
  BALLOON / SCOREINIT / SCOREDIFF, commands matched on the whole name (`#NEXTSONG` used to be read as `#N` and broke
  49 songs). Not supported: bombs, SCOREMODE, LIFE, EXAM, `#NEXTSONG` (dan uses `dan.json`).
- **osu!taiko** charts can go straight into the song library: the course comes from the difficulty name (else OD), the
  level from note density, spinners become balloons and kiai becomes GOGO.
- **MIDI drums**: a MIDI tab in the settings with a device picker and a learn-the-mapping step (not yet tested on a real
  MIDI drum).
- **USIO arcade I/O** (`[usio] enabled`, off by default; experimental, not tested on a real cabinet): drum sensors with
  thresholds, coin and service credits, a test panel. `bnusio.dll` is not included. See [docs/ZDON_USIO.md](docs/ZDON_USIO.md).
- The input test screen shows the input thread's polling rate.
- Chart paths with kana or Hangul open from the command line on Windows.

### Online

- **Self-hosted server** ([`server/`](server/README.md), Python standard library only): the game's online API with signed
  requests, update channels, skin hosting, the website, and admin commands (`publish`, `publish-skin`, `publish-web`, …).
  Set `[network] zdon_server_url` (empty = the server compiled into the build, or offline).
- **Delta updates**: the game checks in the background (`zdon_update_check`); the title screen shows the new version and
  F9 updates and restarts. Files are compared by SHA-256 and downloaded gzip-compressed; settings, scores, cache, songs
  and recordings are never touched. Windows, Linux and macOS; Android updates itself from its own channel.
- **Skins** update from the Z-DON server when one is set.
- **Online battle** (main menu, needs an account):
  - Rooms with a 4-digit code, or quick match; best of three, the loser picks the next song.
  - The guest follows the host's song and picks its own course and options; a song the guest doesn't have is sent
    automatically (up to 85 MB, hash-checked).
  - Hits are sent over WebSocket and over a direct UDP connection (hole punching) at the same time; both sides start
    on a synchronised clock (within 3 ms).
  - Chat (Y), surrender, report (R), a credit score (leaving costs points; low scores lose quick match, then online).
- **Challenges**: play the same chart against someone's recorded play.
- **Website** (zdon.taiko.ac): downloads, song and chart pages, rating leaderboard, difficulty tables (data from
  YataiDON-Syakai), player pages (nameplate, crown counts, best 50, Dan), and "my page" (title, nameplate, character
  look, privacy, score management and CSV export). Sign-in through TaikoForge.
- **GitHub rolling release**: Windows, Linux, macOS and Android builds are published together.

### Web version

Built with Emscripten to WebAssembly and playable in the browser (<https://zdon.taiko.ac/play/>), with a touch drum on
phones and audio mixed in an AudioWorklet for low latency. It is developed in a separate repository; portability fixes
found there come back to Z-DON.

## Where Z-DON does things differently from upstream

Upstream code that Z-DON did not adopt, or replaced with its own:

- **Judging**: upstream's JudgeCore / GaugeCore refactor (e56c73e) is not used; Z-DON keeps its own Player and gauge
  code.
- **Song info**: upstream's Lua `song_info.lua` port is not used; the C++ version stays.
- **Pause menu**: Z-DON's pause menu replaces upstream's (skins with `pause_menu.lua` still draw it), and the Space
  pause key that upstream removed is kept.
- **AI battle**: two systems coexist, chosen by the skin (see [AI battle](#ai-battle)).
- **Drumroll counter**: upstream's Lua counter is used, except in skins with `bubble_unfold` or in Z-DON's AI arena,
  where Z-DON's C++ one draws.
- **Note scrolling**: Z-DON's arcade spacing instead of upstream's `scroll_distance()`.
- **Updates**: Z-DON's own delta updater and server; upstream's `update.ps1` (pointing at YataiDON releases) is not
  shipped.
- **Rendering**: Z-DON's drawing and performance work is built on the OpenGL (raylib) path, not upstream's bgfx path.
- **Navigation loader**: not built, so upstream's waiwai and AI rank-10 assigned folders are not available.
- Known visual differences that come from upstream itself: mirrored textures are 0.5 px off, practice plays the
  intro, and some glow / firework effects changed when upstream removed Lua additive blending (Z-DON skins can opt
  back in with `blend="add"`).

Tried and dropped (measured, no gain or a regression): single-threaded sequential reads, Lua bytecode cache, PGO
releases, a runtime texture atlas, a compiled-chart cache, input timestamps / audio-clock interpolation (would change
judging), streaming songs instead of decoding them fully.

## Bugs fixed

Most of these exist upstream too:

- Crash opening a category whose song folder has subfolders.
- Songs with the same hash or title opened a random one of the files.
- Chart paths with kana / Hangul could not be opened from the command line.
- Crash on exit after practice mode.
- Uninitialised timeline / drumroll colour fields.
- Auto play or a replay in dan saved results and upgraded the nameplate.
- Auto play swallowed hit sounds after frame stalls at 30 / 60 fps.
- `#NEXTSONG` / `#NOTATIONOFF` parsed as `#N`.
- The pause menu took drum presses left over from auto play.
- AI song select crashed on freed animations.
- Hidden test windows were shown (SDL ignored the hidden flag).

## System requirements

- **Windows**: Windows 10 or later (the main development and test platform); OpenGL 3.3, and 4.2 for BC7 textures
- **Linux / macOS / Android**: built in CI with Z-DON's changes, but tested less than Windows
- **iOS / PS5 / Switch**: upstream's ports; Z-DON's changes aim to be cross-platform but are not built for them here
- **Browser**: a recent Chrome / Edge / Firefox with WebAssembly and AudioWorklet

## Installation

1. Download [Z-DON-windows.zip](https://github.com/GanYiZhong/Z-DON/releases/latest/download/Z-DON-windows.zip) (or from <https://zdon.taiko.ac>) and unzip it. It is a rolling
   release: there are no version numbers, the [`rolling`](https://github.com/GanYiZhong/Z-DON/releases/tag/rolling)
   release is replaced by every new build.
2. Run `YataiDON.exe`. On first run, pick the skins to download.
3. Put TJA (or osu!taiko) songs in `Songs`, or set your song folders in the settings. Later updates are one F9 away on
   the title screen.

If latency is high, change `device_type` in the settings to `WDM-KS`, `WASAPI` or `ASIO`.

## Settings

Z-DON's `config.toml` keys (all optional; missing keys take the default):

| Section | Key | Default | What it does |
|---|---|---|---|
| `[general]` | `pause_menu` | `true` | Pause menu during play |
| `[general]` | `zdon_language_auto` | `true` on a new config | Follow the system language |
| `[general]` | `zdon_hit_timing` / `zdon_hit_timing_show` | `true` / `"half"` | Per-hit timing display (`half`, `ok`, `all`) |
| `[general]` | `zdon_song_progress` | `true` | Song progress for skins |
| `[general]` | `zdon_sort_filter` | `true` | Difficulty folders list only uncleared charts |
| `[general]` | `zdon_practice_loop` | `true` | A–B loops in practice |
| `[general]` | `zdon_practice_metronome` | `false` | Metronome in practice |
| `[general]` | `zdon_lane3d` / `zdon_lane3d_preset` | `false` / `0` | 3D view and its camera preset |
| `[general]` | `zdon_hit_error_bar` | `false` | Timing bar under the judge circle |
| `[general]` | `zdon_target_score` | `0` | Target score chip (0 = off) |
| `[general]` | `zdon_new_best_chip` | `false` | "New best +N" on results |
| `[general]` | `zdon_hist_auto_open` | `false` | Results timing panel starts open |
| `[general]` | `zdon_ka_mark` | `false` | White cross on ka notes |
| `[general]` | `zdon_remember_difficulty` | `false` | Remember the difficulty per song |
| `[general]` | `zdon_early_hitsound` / `zdon_press_time_judge` / `zdon_branch_by_measure` | `false` | Hit timing options |
| `[general]` | `zdon_coin_mode` / `zdon_coin_credits` | `false` / `0` | Coin mode |
| `[keys]` | `pause_key` | `"space"` | Second pause key |
| `[keys]` | `random_song_key` / `uncleared_song_key` | unbound | Jump to a random / uncleared song |
| `[video]` | `zdon_texture_compression` | `true` | BC7 texture cache |
| `[video]` | `zdon_composited_fullscreen` | `false` | Composited borderless fullscreen (Windows) |
| `[network]` | `zdon_server_url` / `zdon_server_key` | `""` | Z-DON server (empty = built-in or offline) |
| `[network]` | `zdon_update_check` / `zdon_update_url` | `true` / `""` | Update check and channel |
| `[usio]` | `enabled`, `dll`, `threshold`, `thresholds`, `holdoff_ms`, `poll_us` | `false`, `"bnusio.dll"`, `2000`, —, `25`, `1000` | USIO arcade I/O |

The performance features have no settings; if one causes trouble, an environment variable turns it off, for example
`ZDON_BC7=0`, `ZDON_LAZY_UPLOAD=0`, `ZDON_PREDECODE=0`, `ZDON_VBLANK_PACING=0`, `ZDON_VIDEO_YUV=0`, `ZDON_PCM16=0`,
`YATAIDON_TEXPACK=0`, `YATAIDON_NO_READAHEAD=1`, `ZDON_HIT_POLYPHONY=1`, `ZDON_NO_MOUSE_MENUS=1`. The full list is in
the roadmap.

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
| `ZDON_PGO` | `OFF` | Profile-guided optimization (`GEN` / `USE`, see `tools/build_pgo.sh`); no measurable gain, not used for releases |
| `ZDON_FFMPEG_DIR` | — | Use a decode-only FFmpeg built with `tools/build_ffmpeg_min.sh` |
| `ZDON_NO_MANIFEST` | `OFF` | Leave out the Windows manifest (segment heap) |
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
- `YATAIDON_FAKE_KEYS`, `YATAIDON_FAKE_MOUSE`, `YATAIDON_JUMP_SCREEN`, `YATAIDON_FORCE_AUTO`: fake input, jump straight
  to a screen, force auto play.
- `YATAIDON_FRAME_STATS=1`: one line every 2 s with frame times, CPU and memory; `ZDON_FRAME_SPIKE_MS=<ms>` logs
  frames over a threshold; `ZDON_LUA_PROFILE=1` samples Lua; `ZDON_TEX_USE_STATS=1` lists textures that were never drawn.
- Command-line test flags: `--auto`, `--2p`, `--dan`, `--ai`.

Tools in [`tools/`](tools/): video recording, a Lua syntax checker, skin bundling, the minimal FFmpeg builds, key
generation for the server, and the Android signing key.

## Controls

- During play, **Esc** / **Space**: pause menu; **F1**: quick restart
- On any screen, **Esc**: back
- Song select: **Space** to favorite, **Left/Right Ctrl** to skip 7 songs, **F2** to search, **F5** to refresh
- Search panel: **Tab** genre, **F3** difficulty, **PgUp/PgDn** stars, **Enter** jump to the song
- Practice (paused): digits + **Enter** jump to a bar, **F6** fix the branch, **M** last miss
- Results: **ka** opens the timing histogram
- Online battle menu: **Y** to chat, **R** to report
- Title screen, **F9**: update to the latest version
- Menus: left click confirm, right click back, wheel to move; in the 3D view drag / wheel / middle click move the camera
- Drum keys can be changed in the in-game settings or in `config.toml`

## Reporting issues

Please report Z-DON problems in this repository's [Issues](https://github.com/GanYiZhong/Z-DON/issues), not upstream;
problems with YataiDON itself belong in [YataiDON's Issues](https://github.com/Yonokid/YataiDON/issues).

## Acknowledgments

Z-DON is built on the work of [YataiDON](https://github.com/Yonokid/YataiDON) and all its contributors:
IID, mc08, QBaraki, KabanFriends, DragonRatTiger, Komi, sumandrew, WallK, dpkgluci, Ape, chuchy (osu parser),
churrochef (vertical scroll), cainan (3D don chan), RyutoSetsujin, Nyannurs, fehpizza (logo), Radix, Thonktheta,
SomehowScarlet, SunnyDeez. The website's difficulty table data comes from
[YataiDON-Syakai](https://github.com/Yonokid/YataiDON-Syakai). The practice mode is modelled on PaulSimulator's
training mode; some hit timing ideas come from GuminTaiko, and the TJA / osu!taiko additions follow OpenTaiko,
taiko-web and osu!.

Libraries: [raylib](https://www.raylib.com/), [SDL3](https://github.com/libsdl-org/SDL), [SQLite](https://www.sqlite.org/),
[RapidJSON](https://github.com/Tencent/rapidjson), [toml++](https://github.com/marzer/tomlplusplus), [spdlog](https://github.com/gabime/spdlog),
[Lua](https://github.com/marovira/lua), [sol2](https://github.com/ThePhD/sol2), [cpptrace](https://github.com/jeremy-rifkin/cpptrace),
[libsndfile](https://github.com/libsndfile/libsndfile), [libsamplerate](https://github.com/libsndfile/libsamplerate),
[FFmpeg](https://ffmpeg.org/), [PortAudio](http://www.portaudio.com/), [miniz](https://github.com/richgel999/miniz),
[zlib-ng](https://github.com/zlib-ng/zlib-ng), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), [zstd](https://github.com/facebook/zstd),
[cgltf](https://github.com/jkuatz/cgltf), [curl](https://curl.se/).
