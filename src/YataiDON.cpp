#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <iostream>
#include <print>
#include <thread>

#include "libs/asset_cache.h"
#include "libs/audio/audio.h"
#include "libs/density.h"
#include "libs/file/filesystem.h"
#include "libs/file/logging.h"
#include "libs/file/script.h"
#include "libs/fixed_step.h"
#include "libs/gfx.h"
#include "libs/gfx/camera_utils.h"
#include "libs/gfx/text.h"
#include "libs/global_data.h"
#include "libs/input/ime_ui.h"
#include "libs/input/input.h"
#include "libs/math/animation.h"
#include "libs/network/network.h"
#include "libs/network/skin_updater.h"
#include "libs/network/zdon_update.h"
#include "libs/parsers/song_parser.h"
#include "libs/profiler.h"
#include "libs/raylib_ns.h" // Z-DON: rlgl draw counters, hidden-window and vsync hint calls
#include "libs/screen.h"
#include "libs/song_library_loader.h"
#include "libs/test_mode.h"
#include "libs/texture_bc7.h"
#include "libs/texture_lazy.h"
#include "libs/video/webcam.h"
#include "libs/zdon_os.h"
#include "libs/zdon_record.h"
#include "objects/global/chara_3d.h"
#include "platform/platform.h"
#include "platform/platform_emscripten.h"
#include "platform/platform_ios.h"
#include "platform/platform_windows.h"
#include "platform/vblank_pacing.h"

// On iOS SDL must own main() to start UIKit before SDL_Init(VIDEO) can succeed.
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
#include "libs/gfx/sdl_main_compat.h"
#endif

#include "libs/notify.h"
#include "objects/global/debug_menu.h"
#include "objects/global/fps_counter.h"
#include "objects/global/toast.h"
#include "objects/global/zdon_hud.h"
#include "objects/song_select/file_navigator/navigator.h"
#include "scenes/ai_battle.h"
#include "scenes/battle.h"
#include "scenes/campaign.h"
#include "scenes/dan_result.h"
#include "scenes/dan_select.h"
#include "scenes/entry.h"
#include "scenes/game.h"
#include "scenes/game_2p.h"
#include "scenes/game_dan.h"
#include "scenes/game_over.h"
#include "scenes/game_practice.h"
#include "scenes/input_cali.h"
#include "scenes/input_test.h"
#include "scenes/kinotake_result.h"
#include "scenes/loading.h"
#include "scenes/onboarding.h"
#include "scenes/result.h"
#include "scenes/result_2p.h"
#include "scenes/retry_game.h"
#include "scenes/reward_shop.h"
#include "scenes/settings.h"
#include "scenes/song_select.h"
#include "scenes/song_select_2p.h"
#include "scenes/song_select_ai.h"
#include "scenes/song_select_online.h"
#include "scenes/song_select_practice.h"
#include "scenes/song_select_record.h"
#include "scenes/title.h"
#include "scenes/total_result.h"
#include "scenes/tutorial.h"
#include "scenes/waiwai.h"

void draw_outer_border(int screen_width, int screen_height, Color last_color) {
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx != 0 || dy != 0) {
                draw_rectangle(dx * screen_width, dy * screen_height, screen_width, screen_height, last_color);
            }
        }
    }
}

static void draw_skin_update_status() {
    const SkinUpdater::Status st = skin_updater.snapshot();
    if (!st.running) {
        return;
    }

    const int size = static_cast<int>(20.0f * global_tex.screen_scale);
    const float x = static_cast<float>(size);
    float y = static_cast<float>(size) * 2.0f;

    std::string skin_line =
        st.current_skin.empty() ? "Checking for skin updates..." : "Updating skin: " + st.current_skin;
    if (!st.current_skin.empty() && st.files_total > 0) {
        skin_line += " (" + std::to_string(st.files_done) + " / " + std::to_string(st.files_total) + " files)";
    }
    Font skin_font = font_manager.get_font(skin_line, size);
    draw_text_ex(skin_font, skin_line.c_str(), Vec2{x, y}, static_cast<float>(size), 1.0f, Color::YELLOW);

    if (!st.current_file.empty()) {
        y += static_cast<float>(size);
        Font file_font = font_manager.get_font(st.current_file, size);
        draw_text_ex(file_font, st.current_file.c_str(), Vec2{x, y}, static_cast<float>(size), 1.0f, Color::WHITE);
    }
}

// Z-DON: a newer release on the self-hosted server is offered on the title and entry screens; F9 hands over to
// zdon_update.ps1, which waits for the game to close, updates the changed files and starts it again
static bool g_quit_for_update = false;

static bool zdon_update_offered(Screens screen) {
    return (screen == Screens::TITLE || screen == Screens::ENTRY) && zdon_update::status().available;
}

static void draw_zdon_update_notice(Screens screen) {
    if (!zdon_update_offered(screen)) {
        return;
    }
    const zdon_update::Status st = zdon_update::status();
    const float s = tex.screen_scale;
    OutlinedText& t = zhud::label(zhud::text("update_available", st.version, std::to_string(st.files)), 26);
    const float w = t.width + 40 * s;
    const float h = t.height + 16 * s;
    const float x = (tex.screen_width - w) / 2.0f;
    const float y = 24 * s;
    zhud::panel(x, y, w, h, 0.86f, Color{255, 200, 60, 200});
    t.draw({.x = x + 20 * s, .y = y + 8 * s});
}

static void notify_skin_update_finished() {
    static bool was_running = false;
    const SkinUpdater::Status st = skin_updater.snapshot();
    if (was_running && !st.running && st.any_updated && st.failed.empty()) {
        notify("Skin updates installed");
    }
    was_running = st.running;
}

[[noreturn]] static void exit_now(int code) {
    std::cout.flush();
    std::cerr.flush();
    if (auto logger = spdlog::default_logger()) {
        logger->flush();
    }
    std::_Exit(code);
}

static std::filesystem::path path_from_arg(const std::string& arg) {
#ifdef _WIN32
    return win32_path_from_utf8(arg);
#else
    try {
        return std::filesystem::path(arg);
    } catch (const std::exception&) {
        return {};
    }
#endif
}

Screens check_args(int argc, char* argv[]) {
    if (argc == 1) {
        return Screens::LOADING;
    }

    std::string song_path;
    std::optional<int> difficulty;
    bool auto_play = false;
    bool practice = false;
    bool two_player = false;
    bool ai_battle = false; // Z-DON: --ai, the AI battle (2P lane = AI)
    std::string dan_arg;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (zdon_record::consume_arg(arg, i, argc)) {
            continue; // read by zdon_record::prepare
        }
        if (arg == "--auto") {
            auto_play = true;
        } else if (arg == "--practice") {
            practice = true;
        } else if (arg == "--2p") {
            two_player = true;
        } else if (arg == "--ai") {
            ai_battle = true;
        } else if (arg == "--dan") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --dan requires a course path\n";
                exit_now(1);
            }
            dan_arg = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " <song_path> [difficulty] [--auto] [--practice] [--2p]\n";
            std::cout << "       " << argv[0] << " --dan <dan.json or course folder>\n";
            std::cout << "  song_path   : Path to the TJA song file\n";
            std::cout << "  difficulty  : Difficulty level (optional, defaults to max difficulty)\n";
            std::cout << "  --auto      : Enable auto mode\n";
            std::cout << "  --practice  : Start in practice mode\n";
            std::cout << "  --2p        : (test) both players play the song on auto, then the 2P result\n";
            std::cout
                << "  --ai        : play the song against the AI (tuned by the course level); --auto puts 1P on auto\n";
            std::cout << "  --dan <path>: (test) load the library, then play that dan course on auto\n";
            std::cout << "  --record <out.mp4>    : render the play to a video, faster than real time (no hits\n";
            std::cout << "                          unless --auto); --record-fps <n> (60), --record-result <s> (6)\n";
            exit_now(0);
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Error: Unknown option: " << arg << "\n";
            exit_now(1);
        } else if (song_path.empty()) {
            song_path = arg;
        } else if (!difficulty.has_value()) {
            try {
                size_t pos = 0;
                int value = std::stoi(arg, &pos);
                if (pos != arg.size()) {
                    throw std::invalid_argument(arg);
                }
                difficulty = value;
            } catch (const std::exception& e) {
                std::cerr << "Error: Invalid difficulty value: " << arg << "\n";
                exit_now(1);
            }
        } else {
            std::cerr << "Error: Unexpected extra argument: " << arg << "\n";
            exit_now(1);
        }
    }

    if (!dan_arg.empty()) {
        // the course's songs are found through the library: it loads first, then dan select starts the course
        std::filesystem::path dan = path_from_arg(dan_arg);
        std::error_code dan_ec;
        if (dan.empty() || !std::filesystem::exists(dan, dan_ec) || !song_path.empty() || two_player || practice) {
            std::cerr << "Error: --dan takes an existing dan.json or folder, without song_path/--2p/--practice\n";
            exit_now(1);
        }
        global_data.cli_dan = std::filesystem::absolute(dan, dan_ec);
        global_data.force_auto_play = true;
        return Screens::LOADING;
    }

    if (song_path.empty()) {
        std::cerr << "Error: song_path is required\n";
        std::cerr << "Use --help for usage information\n";
        exit_now(1);
    }

    std::filesystem::path path = path_from_arg(song_path);
    if (path.empty()) {
        std::cerr << "Error: Song path could not be interpreted: " << song_path << "\n";
        exit_now(1);
    }
    std::error_code path_ec;
    if (!std::filesystem::exists(path, path_ec) || path_ec) {
        std::cerr << "Error: Song file not found: " << song_path << "\n";
        exit_now(1);
    }

    {
        std::error_code abs_ec;
        std::filesystem::path abs = std::filesystem::absolute(path, abs_ec);
        if (!abs_ec) {
            path = abs;
        }
    }
    SongParser tja = [&] -> SongParser {
        try {
            return SongParser(path);
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to parse song file: " << e.what() << "\n";
            exit_now(1);
        }
    }();

    int selected_difficulty;
    if (difficulty.has_value()) {
        auto& course_data = tja.metadata.course_data;
        if (!course_data.contains(difficulty.value())) {
            std::cerr << "Error: Invalid difficulty: " << difficulty.value() << ". Available: ";
            for (const auto& [key, value] : course_data) {
                std::cerr << key << " ";
            }
            std::cerr << "\n";
            exit_now(1);
        }
        selected_difficulty = difficulty.value();
    } else {
        if (tja.metadata.course_data.empty()) {
            selected_difficulty = static_cast<int>(Difficulty::EASY);
        } else {
            selected_difficulty = tja.metadata.course_data.rbegin()->first;
        }
    }

    // Z-DON: --record-tracks, a lane per course (zdon_record.h)
    const std::vector<int>& record_tracks = zdon_record::tracks();
    if (!record_tracks.empty()) {
        if (record_tracks.size() > 5 || two_player || practice || ai_battle) {
            std::cerr << "Error: --record-tracks takes 1 to 5 courses, without --2p/--practice/--ai\n";
            exit_now(1);
        }
        for (const int course : record_tracks) {
            if (!tja.metadata.course_data.contains(course)) {
                std::cerr << "Error: Invalid difficulty in --record-tracks: " << course << "\n";
                exit_now(1);
            }
        }
        selected_difficulty = record_tracks[0];
    }

    if (ai_battle && (two_player || practice)) {
        std::cerr << "Error: --ai cannot be combined with --2p or --practice\n";
        exit_now(1);
    }
    if (two_player && practice) {
        std::cerr << "Error: --2p and --practice cannot be combined\n";
        exit_now(1);
    }
    Screens current_screen = practice ? Screens::GAME_PRACTICE : Screens::GAME;
    if (two_player) {
        current_screen = Screens::GAME_2P;
        global_data.session_data[static_cast<int>(PlayerNum::P2)].selected_song = path;
        global_data.session_data[static_cast<int>(PlayerNum::P2)].selected_difficulty = selected_difficulty;
        auto_play = true;
    }
    if (record_tracks.size() == 2) { // 1P and 2P, without the --2p auto play
        current_screen = Screens::GAME_2P;
        global_data.session_data[static_cast<int>(PlayerNum::P2)].selected_song = path;
        global_data.session_data[static_cast<int>(PlayerNum::P2)].selected_difficulty = record_tracks[1];
    }
    if (ai_battle) {
        current_screen = Screens::GAME_2P; // Game2PScreen copies 1P's song to the AI's lane
        global_data.zdon_ai.requested = true;
    }
    global_data.session_data[static_cast<int>(PlayerNum::P1)].selected_song = std::move(path);
    global_data.session_data[static_cast<int>(PlayerNum::P1)].selected_difficulty = selected_difficulty;
    if (auto_play) {
        global_data.force_auto_play = true;
    }

    return current_screen;
}

struct LoopState {
    std::unordered_map<Screens, std::unique_ptr<Screen>> screens;
    Screens current_screen = Screens::LOADING;
    Camera2D camera;
    std::chrono::steady_clock::duration target_duration =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / 60.0));
    std::chrono::time_point<std::chrono::steady_clock> next_frame_time = std::chrono::steady_clock::now();
    FPSCounter fps_counter;
    std::unique_ptr<Toast> toast;
    Color last_color = Color::BLACK;
    double screen_fade_start = 0.0;
};

static bool screen_fade_applies(Screens from, Screens to) {
    switch (to) {
    case Screens::GAME:
    case Screens::GAME_2P:
    case Screens::GAME_DAN:
    case Screens::TUTORIAL:
    case Screens::GAME_PRACTICE:
    case Screens::AI_GAME:
    case Screens::AI_RESULT:
    case Screens::BATTLE_GAME:
    case Screens::BATTLE_RESULT:
    case Screens::RESULT:
    case Screens::RESULT_2P:
    case Screens::WAIWAI_GAME:
    case Screens::WAIWAI_RESULT:
    case Screens::DAN_RESULT:
        return false;
    default:
        break;
    }
    if (from == Screens::LOADING) {
        return false;
    }
    if (to == Screens::DAN_SELECT && (from == Screens::SONG_SELECT || from == Screens::ENTRY)) {
        return false;
    }
    return true;
}

static LoopState* g_loop = nullptr;
double g_frame_ms = 0.0;

template<typename T>
static void put_screen(std::unordered_map<Screens, std::unique_ptr<Screen>>& screens, std::optional<Screens> except,
                       Screens id) {
    if (except != id) {
        screens[id] = std::make_unique<T>();
    }
}

static void populate_screens(std::unordered_map<Screens, std::unique_ptr<Screen>>& screens,
                             std::optional<Screens> except = std::nullopt) {
    put_screen<EntryScreen>(screens, except, Screens::ENTRY);
    put_screen<TitleScreen>(screens, except, Screens::TITLE);
    put_screen<SongSelectScreen>(screens, except, Screens::SONG_SELECT);
    put_screen<SongSelect2PScreen>(screens, except, Screens::SONG_SELECT_2P);
    put_screen<WaiwaiSongSelectScreen>(screens, except, Screens::WAIWAI_SONG_SELECT);
    put_screen<WaiwaiGameScreen>(screens, except, Screens::WAIWAI_GAME);
    put_screen<WaiwaiResultScreen>(screens, except, Screens::WAIWAI_RESULT);
    put_screen<WaiwaiTutorialScreen>(screens, except, Screens::WAIWAI_TUTORIAL);
    put_screen<WaiwaiRetryGameScreen>(screens, except, Screens::WAIWAI_RETRY_GAME);
    // merge 0ed42ec: two AI battles share Screens::AI_SELECT. A skin whose own Scripts folder has upstream's
    // ai_support_menu Lua script gets upstream's (Lua, server-backed) AISongSelectScreen -> AI_GAME -> AI_RESULT;
    // any other skin, also a child that only inherits the script (Nijiiro, YataiDON14 from Green), gets Z-DON's
    // cabinet AI battle (ZdonAiSongSelectScreen -> GAME_2P / RESULT_2P with global_data.zdon_ai). The skin's
    // scripts are loaded before this runs, and a skin change repopulates the screens (reload_skin_screens).
    if (zdon_ai_upstream()) {
        put_screen<AISongSelectScreen>(screens, except, Screens::AI_SELECT);
    } else {
        put_screen<ZdonAiSongSelectScreen>(screens, except, Screens::AI_SELECT);
    }
    put_screen<AIGameScreen>(screens, except, Screens::AI_GAME);
    put_screen<AIResultScreen>(screens, except, Screens::AI_RESULT);
    put_screen<BattleSongSelectScreen>(screens, except, Screens::BATTLE_SELECT); // upstream enso battle
    put_screen<BattleGameScreen>(screens, except, Screens::BATTLE_GAME);
    put_screen<BattleResultScreen>(screens, except, Screens::BATTLE_RESULT);
    put_screen<LoadingScreen>(screens, except, Screens::LOADING);
    put_screen<GameScreen>(screens, except, Screens::GAME);
    put_screen<Game2PScreen>(screens, except, Screens::GAME_2P);
    put_screen<PracticeGameScreen>(screens, except, Screens::GAME_PRACTICE);
    put_screen<PracticeSongSelectScreen>(screens, except, Screens::PRACTICE_SELECT);
    put_screen<RecordSongSelectScreen>(screens, except, Screens::RECORD_SELECT); // Z-DON recording mode
    put_screen<OnlineSongSelectScreen>(screens, except, Screens::ONLINE_SELECT); // Z-DON online battle
    put_screen<ResultScreen>(screens, except, Screens::RESULT);
    put_screen<Result2PScreen>(screens, except, Screens::RESULT_2P);
    put_screen<DanSelectScreen>(screens, except, Screens::DAN_SELECT);
    put_screen<DanGameScreen>(screens, except, Screens::GAME_DAN);
    put_screen<DanResultScreen>(screens, except, Screens::DAN_RESULT);
    put_screen<SettingsScreen>(screens, except, Screens::SETTINGS);
    put_screen<InputCaliScreen>(screens, except, Screens::INPUT_CALI);
    put_screen<GameOverScreen>(screens, except, Screens::GAME_OVER);
    put_screen<InputTestScreen>(screens, except, Screens::INPUT_TEST);
    put_screen<RetryGameScreen>(screens, except, Screens::RETRY_GAME);
    put_screen<OnboardingScreen>(screens, except, Screens::ONBOARDING);
    put_screen<TutorialScreen>(screens, except, Screens::TUTORIAL);
    put_screen<CampaignScreen>(screens, except, Screens::CAMPAIGN);
    put_screen<KinotakeResultScreen>(screens, except, Screens::KINOTAKE_RESULT);
    put_screen<TotalResultScreen>(screens, except, Screens::TOTAL_RESULT);
    put_screen<RewardShopScreen>(screens, except, Screens::REWARD_SHOP);
}

void drop_other_screens_for_skin_reload() {
    if (!g_loop) {
        return;
    }
    g_loop->toast.reset(); // holds Lua refs; must die before the state does
    for (auto it = g_loop->screens.begin(); it != g_loop->screens.end();) {
        if (it->first != g_loop->current_screen) {
            it = g_loop->screens.erase(it);
        } else {
            ++it;
        }
    }
}

void reload_skin_screens() {
    if (!g_loop) {
        return;
    }
    populate_screens(g_loop->screens, g_loop->current_screen);
}

// Index of the frame being rendered (0 = the first). YATAIDON_FIXED_STEP_MS (libs/fixed_step.h):
// the frame's clock is start + index * step, and YATAIDON_JUMP_SCREEN / YATAIDON_FAKE_KEYS /
// YATAIDON_DUMP_FRAMES count frames.
static long long g_frame_index = -1;
static void dev_frame_begin() {
    g_frame_index++;
    if (fixed_step::on()) {
        fixed_step::begin_frame();
    }
}
// the dev hooks' clock: frames in fixed-step mode, else ms since the first frame
static double dev_hook_clock() {
    static const auto start = std::chrono::steady_clock::now();
    if (fixed_step::on()) {
        return static_cast<double>(g_frame_index);
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

static bool dump_screen_tick();   // dev hook, defined below
static bool draw_log_dev_hook() { // ZDON_DRAW_LOG=1: YATAIDON_DUMP_* frames also get <frame>.draws.tsv
    static const bool on = std::getenv("ZDON_DRAW_LOG") != nullptr;
    return on;
}
static void write_draw_log(const std::string& path);
static bool g_dump_screen_done = false; // the dump hook's last shot is taken: leave the main loop

// Dev hook: YATAIDON_FRAME_STATS=1 logs, every 2 s, the CPU time each frame spent before present
// (input, update, draw submission; not the vsync / frame-limiter wait).
// ZDON_FRAME_SPIKE_MS=<ms> (with or without FRAME_STATS) logs every single frame whose work took longer
// than that, split into screen update / draw, and every present (EndDrawing + swap) longer than that, so
// a hitch can be matched with the log lines around it.
// Segment marks of the current frame, for the spike log: where the time outside update/draw went.
// `overlay` includes EndMode2D's batch flush, where a driver stall shows up (one of ~300 ms about 0.3-1 s
// after start, before any screen loads: not ours); the PNG write of YATAIDON_DUMP_SCREEN is outside them all.
enum SpikeSeg {
    SEG_START,
    SEG_INPUT,
    SEG_PRE_UPDATE,
    SEG_UPDATE,
    SEG_HOOKS,
    SEG_DRAW,
    SEG_SWITCH,
    SEG_OVERLAY,
    SEG_COUNT
};
static std::chrono::steady_clock::time_point g_seg[SEG_COUNT];
static void seg_mark(SpikeSeg s) {
    g_seg[s] = std::chrono::steady_clock::now();
}
static double g_spike_update_ms = 0, g_spike_draw_ms = 0; // this frame's screen update / draw, for the spike log
static std::string g_spike_screen;                        // copied: a skin reload may delete the screen before the log
static uint64_t g_spike_frame_no = 0;
static double frame_spike_ms() {
    static const double ms = [] {
        const char* v = std::getenv("ZDON_FRAME_SPIKE_MS");
        return v && *v ? std::atof(v) : 0.0;
    }();
    return ms;
}
// the present (EndDrawing + swap) of a frame: where the driver waits for the work the frame queued
static void frame_spike_present(std::chrono::steady_clock::time_point start) {
    const double spike_ms = frame_spike_ms();
    if (spike_ms <= 0) {
        return;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (ms > spike_ms) {
        spdlog::info("[spike] frame {} present {:.2f} ms on {}", g_spike_frame_no, ms, g_spike_screen);
    }
}
static void frame_stats_add(std::chrono::steady_clock::time_point start) {
    static const bool on = [] {
        const char* v = std::getenv("YATAIDON_FRAME_STATS");
        return v && *v == '1';
    }();
    const double spike_ms = frame_spike_ms();
    if (!on && spike_ms <= 0) {
        return;
    }
    uint64_t& frame_no = g_spike_frame_no;
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - start).count();
    frame_no++;
    if (spike_ms > 0 && ms > spike_ms) {
        auto seg = [](SpikeSeg a, SpikeSeg b) {
            return g_seg[b] >= g_seg[a] ? std::chrono::duration<double, std::milli>(g_seg[b] - g_seg[a]).count() : -1.0;
        };
        spdlog::info("[spike] frame {} work {:.2f} ms (update {:.2f}, draw {:.2f}) on {}; input {:.2f} pre {:.2f} "
                     "hooks {:.2f} switch {:.2f} overlay {:.2f}",
                     frame_no, ms, g_spike_update_ms, g_spike_draw_ms, g_spike_screen, seg(SEG_START, SEG_INPUT),
                     seg(SEG_INPUT, SEG_PRE_UPDATE), seg(SEG_UPDATE, SEG_HOOKS), seg(SEG_DRAW, SEG_SWITCH),
                     seg(SEG_SWITCH, SEG_OVERLAY));
    }
    if (!on) {
        return;
    }
    static std::vector<double> work;
    static auto window = std::chrono::steady_clock::now();
    // frame start-to-start intervals over 2.08 / 4.17 / 8.33 ms (a 480 / 240 / 120 Hz refresh)
    static std::chrono::steady_clock::time_point prev_start{};
    static int over[3] = {};
    if (prev_start.time_since_epoch().count() != 0) {
        const double interval = std::chrono::duration<double, std::milli>(start - prev_start).count();
        over[0] += interval > 2.08;
        over[1] += interval > 4.17;
        over[2] += interval > 8.33;
    }
    prev_start = start;
    work.push_back(ms);
    if (now - window >= std::chrono::seconds(2)) {
        std::ranges::sort(work);
        double sum = 0;
        for (double v : work) {
            sum += v;
        }
        std::string usage;
#ifdef _WIN32
        // process CPU % of one core over the window, working set and private bytes
        static double prev_cpu_ms = -1;
        double cpu_ms = 0, ws_mb = 0, private_mb = 0;
        if (win32_process_usage(cpu_ms, ws_mb, private_mb)) {
            const double wall_ms = std::chrono::duration<double, std::milli>(now - window).count();
            usage = prev_cpu_ms < 0 ? std::format("; ws {:.0f} MB private {:.0f} MB", ws_mb, private_mb)
                                    : std::format("; cpu {:.1f}% ws {:.0f} MB private {:.0f} MB",
                                                  100.0 * (cpu_ms - prev_cpu_ms) / wall_ms, ws_mb, private_mb);
            prev_cpu_ms = cpu_ms;
        }
#endif
        spdlog::info("[frame] {} frames, work ms: avg {:.3f} p50 {:.3f} p99 {:.3f} max {:.3f}; intervals >2.08 {} "
                     ">4.17 {} >8.33 {}{}",
                     work.size(), sum / work.size(), work[work.size() / 2], work[work.size() * 99 / 100], work.back(),
                     over[0], over[1], over[2], usage);
        work.clear();
        over[0] = over[1] = over[2] = 0;
        window = now;
    }
}

// Dev stat YATAIDON_DRAW_STATS=1: GL work per frame from rlgl's counters (cmake/patches/raylib-lazy-blend.patch),
// logged every 2 s: batch flushes with vertices, draw calls, blend switches, vertex-array (3D mesh) draws.
// ZDON_EAGER_BLEND=1 switches blend modes at once like stock raylib (for comparison).
static bool draw_stats_on() {
    static const bool on = [] {
        const char* e = std::getenv("ZDON_EAGER_BLEND");
        if (e && *e == '1') {
            ray::rlSetBlendModeLazy(false);
        }
        const char* v = std::getenv("YATAIDON_DRAW_STATS");
        return v && *v == '1';
    }();
    return on;
}

static void draw_stats_add(const std::string& screen_name) {
    if (!draw_stats_on()) {
        return;
    }
    static unsigned int last[4] = {};
    static unsigned long long sum[4] = {};
    static unsigned int peak[4] = {};
    static unsigned int frames = 0;
    static auto window = std::chrono::steady_clock::now();
    unsigned int now_stats[4];
    ray::rlGetDrawStats(now_stats);
    for (int i = 0; i < 4; i++) {
        const unsigned int d = now_stats[i] - last[i];
        last[i] = now_stats[i];
        sum[i] += d;
        peak[i] = std::max(peak[i], d);
    }
    frames++;
    const auto now = std::chrono::steady_clock::now();
    if (now - window >= std::chrono::seconds(2)) {
        const double n = frames;
        spdlog::info("[draw] {} {} frames per frame avg/max: flushes {:.1f}/{} draw calls {:.1f}/{} blend switches "
                     "{:.1f}/{} mesh draws {:.1f}/{}",
                     screen_name, frames, sum[0] / n, peak[0], sum[1] / n, peak[1], sum[2] / n, peak[2], sum[3] / n,
                     peak[3]);
        for (int i = 0; i < 4; i++) {
            sum[i] = 0;
            peak[i] = 0;
        }
        frames = 0;
        window = now;
    }
}

void reload_current_screen_scripts() {
    if (!g_loop) {
        return;
    }
    // Every screen holds sol::tables created by the state we are about to replace,
    // and the navigator keeps its own Lua-backed boxes across screens.
    g_loop->screens.clear();
    g_loop->toast.reset();
    navigator.reset_for_skin_reload();
    script_manager.shutdown();
    script_manager.init(fs::path("Skins") / global_data.config->paths.skin / "Scripts");
    populate_screens(g_loop->screens);
}

static void apply_pending_skin_reload() {
    if (!global_data.skin_reload_pending) {
        return;
    }
    global_data.skin_reload_pending = false;
    spdlog::info("Reloading for this credit: skin {}, language {}", global_data.config->paths.skin.string(),
                 global_data.config->general.language);
    g_loop->screens.clear();
    g_loop->toast.reset();
    try {
        load_skin();
    } catch (const std::exception& e) {
        // The folder is there but unusable: lose the account's skin rather than the cabinet.
        spdlog::error("Skin reload failed ({}); falling back to the instance skin", e.what());
        revert_user_appearance();
        global_data.skin_reload_pending = false;
        load_skin();
    }
    populate_screens(g_loop->screens);
}

static std::chrono::steady_clock::duration frame_duration(double fps) {
    return fps > 0.0 && !fixed_step::on() ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                std::chrono::duration<double>(1.0 / fps))
                                          : std::chrono::steady_clock::duration::zero();
}

static void run_frame() {
    PROFILE_FRAME();
    PROFILE_SCOPE();
    LoopState& L = *g_loop;
    dev_frame_begin(); // frame index; YATAIDON_FIXED_STEP_MS: this frame's clock
    draw_stats_on();   // reads ZDON_EAGER_BLEND before the first draw
    const auto frame_start = std::chrono::steady_clock::now();
    seg_mark(SEG_START);
    // Settings can change the target frame rate while running; the platform may adjust it
    // (written back so settings show what is in effect).
    static int applied_fps = global_data.config->video.target_fps;
    if (global_data.config->video.target_fps != applied_fps) {
        applied_fps = global_data.config->video.target_fps =
            platform_apply_target_fps(global_data.config->video.target_fps);
        L.target_duration = frame_duration(applied_fps);
        L.next_frame_time = std::chrono::steady_clock::now();
    }

    g_frame_ms = vblank_pacing::frame_ms(get_current_ms());
    L.fps_counter.update();

    platform_poll_input();
    poll_input_events();
    if (platform_is_suspended()) {
        return;
    }
#ifndef YATAIDON_INPUT_THREAD
    poll_keyboard_once(); // no input thread polling it
#endif
    poll_touch_once();

    apply_queued_window_resize();
    seg_mark(SEG_INPUT);

    if (check_key_pressed(global_data.config->keys.fullscreen_key)) {
        toggle_fullscreen();
        spdlog::info("Toggled fullscreen");
    } else if (check_key_pressed(global_data.config->keys.borderless_key)) {
        toggle_borderless_windowed();
        spdlog::info("Toggled borderless windowed mode");
    }

    apply_pending_skin_reload();

    // Z-DON texture density: with a skin's texture_density > 1 the frame draws into a target of the screen size
    // x density from here on, and the camera below maps onto it
    density::begin_frame(global_data.camera.border_color);
    L.camera = compute_camera2d(tex.screen_width, tex.screen_height);
    if (!global_data.skinless) {
        if (!L.toast && script_manager.has_lua_script("toast")) {
            L.toast = std::make_unique<Toast>();
        }
        if (L.toast) {
            L.toast->update(g_frame_ms);
        }
        notify_skin_update_finished();
        debug_menu.update(L.camera);
    }
    if (draw_log_dev_hook()) { // dev hook ZDON_DRAW_LOG: keep this frame's draw calls for dump_screen_tick
        debug_draw_log.clear();
        debug_log_draws = true;
    }

    begin_drawing();

    if (global_data.camera.border_color != L.last_color) {
        clear_background(global_data.camera.border_color);
        L.last_color = global_data.camera.border_color;
    }

    begin_mode_2d(L.camera);
    begin_blend_mode(blend_mode::CUSTOM_SEPARATE);

    auto screen_it = L.screens.find(L.current_screen);
    Screen* screen = (screen_it != L.screens.end()) ? screen_it->second.get() : nullptr;
    if (!screen) {
        static Screens last_logged_screen = L.current_screen;
        static double last_log_ms = -1e9;
        if (L.current_screen != last_logged_screen || g_frame_ms - last_log_ms > 1000.0) {
            spdlog::error("Active screen {} is not available, attempting recovery", L.current_screen);
            last_logged_screen = L.current_screen;
            last_log_ms = g_frame_ms;
        }
        populate_screens(L.screens);
        screen_it = L.screens.find(L.current_screen);
        screen = (screen_it != L.screens.end()) ? screen_it->second.get() : nullptr;
    }
    if (!screen) {
        end_blend_mode();
        end_mode_2d();
        density::end_frame(global_data.camera.border_color);
        end_drawing();
        return;
    }

    network.update(g_frame_ms);
    seg_mark(SEG_PRE_UPDATE);
    std::optional<Screens> next_screen;
    {
        PROFILE_SCOPE_N("screen update");
        PROFILE_ZONE_TEXT(screen->screen_name);
        const auto t0 = std::chrono::steady_clock::now();
        if (screen->in_game_over()) {
            // The game over overlay plays over the live host screen: keep the host updating so it is
            // not frozen under the overlay, and stay in the scene until the overlay finishes.
            screen->update();
            next_screen = screen->update_game_over();
        } else {
            next_screen = screen->update();
        }
        g_spike_update_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        g_spike_screen = screen->screen_name;
    }
    seg_mark(SEG_UPDATE);
    sync_text_input();

    // Dev hook: YATAIDON_JUMP_SCREEN="<entry|song_select|dan_select|title|practice_select|settings|tutorial>;<ms>"
    // jumps there once, <ms> after the first frame (with YATAIDON_DUMP_SCREEN: screens the CLI can't start in)
    {
        const double since = dev_hook_clock(); // frames with YATAIDON_FIXED_STEP_MS
        static bool jump_done = false;
        static const char* jump = std::getenv("YATAIDON_JUMP_SCREEN");
        if (!jump_done && jump && *jump) {
            std::string j(jump);
            size_t semi = j.find(';');
            double at = semi == std::string::npos ? 3000.0 : std::atof(j.substr(semi + 1).c_str());
            std::string name = j.substr(0, semi);
            if (since >= at) {
                jump_done = true;
                if (name == "entry") {
                    debug_menu.requested_screen = Screens::ENTRY;
                } else if (name == "song_select") {
                    debug_menu.requested_screen = Screens::SONG_SELECT;
                } else if (name == "dan_select") {
                    debug_menu.requested_screen = Screens::DAN_SELECT;
                } else if (name == "title") {
                    debug_menu.requested_screen = Screens::TITLE;
                } else if (name == "practice_select") {
                    debug_menu.requested_screen = Screens::PRACTICE_SELECT;
                } else if (name == "ai_select") {
                    debug_menu.requested_screen = Screens::AI_SELECT;
                } else if (name == "record_select") {
                    debug_menu.requested_screen = Screens::RECORD_SELECT;
                } else if (name == "online_select") {
                    debug_menu.requested_screen = Screens::ONLINE_SELECT;
                } else if (name == "settings") {
                    debug_menu.requested_screen = Screens::SETTINGS;
                } else if (name == "tutorial") {
                    debug_menu.requested_screen = Screens::TUTORIAL;
                }
            }
        }
        // Dev hook: YATAIDON_FAKE_KEYS="<ms>:<raylib key code>,..." presses each key once, <ms> after
        // the first frame, through the game's own input queue (no OS input: other windows unaffected)
        static const char* fake = std::getenv("YATAIDON_FAKE_KEYS");
        static std::vector<std::pair<double, int>> fake_keys;
        static bool fake_parsed = false;
        if (fake && *fake && !fake_parsed) {
            fake_parsed = true;
            std::string f(fake);
            size_t pos = 0;
            while (pos < f.size()) {
                size_t comma = f.find(',', pos);
                std::string item = f.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                size_t colon = item.find(':');
                if (colon != std::string::npos) {
                    fake_keys.emplace_back(std::atof(item.substr(0, colon).c_str()),
                                           std::atoi(item.substr(colon + 1).c_str()));
                }
                if (comma == std::string::npos) {
                    break;
                }
                pos = comma + 1;
            }
        }
        // Z-DON dev hook: YATAIDON_FAKE_MOUSE="<ms>:<x>:<y>:<button>:<wheel>,..." (render px; button 0 = move)
        static const char* fake_mouse = std::getenv("YATAIDON_FAKE_MOUSE");
        static std::vector<std::array<double, 5>> fake_mice;
        static bool fake_mouse_parsed = false;
        if (fake_mouse && *fake_mouse && !fake_mouse_parsed) {
            fake_mouse_parsed = true;
            std::string f(fake_mouse);
            for (size_t pos = 0; pos < f.size();) {
                size_t comma = f.find(',', pos);
                std::string item = f.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                std::array<double, 5> v = {};
                std::sscanf(item.c_str(), "%lf:%lf:%lf:%lf:%lf", &v[0], &v[1], &v[2], &v[3], &v[4]);
                fake_mice.push_back(v);
                if (comma == std::string::npos) {
                    break;
                }
                pos = comma + 1;
            }
        }
        for (auto it = fake_mice.begin(); it != fake_mice.end();) {
            if (since >= (*it)[0]) {
                spdlog::info("[devmouse] {:.0f},{:.0f} button {} wheel {} at {:.0f}", (*it)[1], (*it)[2], (*it)[3],
                             (*it)[4], since);
                dev_inject_mouse(static_cast<float>((*it)[1]), static_cast<float>((*it)[2]), static_cast<int>((*it)[3]),
                                 static_cast<float>((*it)[4]));
                it = fake_mice.erase(it);
            } else {
                ++it;
            }
        }
        ime_ui_dev_tick(since);
        // Z-DON dev hook: YATAIDON_FAKE_MIDI="<ms>:<note>[:<velocity>],..." plays MIDI note-ons as a drum would
        static std::vector<std::array<int, 3>> fake_midi = [] {
            std::vector<std::array<int, 3>> v;
            if (const char* e = std::getenv("YATAIDON_FAKE_MIDI")) {
                std::string f(e);
                for (size_t pos = 0; pos < f.size();) {
                    const size_t comma = f.find(',', pos);
                    const std::string item =
                        f.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    std::array<int, 3> m = {0, 0, 100};
                    std::sscanf(item.c_str(), "%d:%d:%d", &m[0], &m[1], &m[2]);
                    v.push_back(m);
                    if (comma == std::string::npos) {
                        break;
                    }
                    pos = comma + 1;
                }
            }
            return v;
        }();
        for (auto it = fake_midi.begin(); it != fake_midi.end();) {
            if (since >= (*it)[0]) {
                spdlog::info("[devmidi] note {} velocity {} at {:.0f}", (*it)[1], (*it)[2], since);
                dev_inject_midi_note((*it)[1], (*it)[2]);
                it = fake_midi.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = fake_keys.begin(); it != fake_keys.end();) {
            if (since >= it->first) {
                spdlog::info("[devkeys] key {} at {:.0f}", it->second, since);
                dev_inject_key(it->second);
                it = fake_keys.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (debug_menu.reload_scripts) {
        debug_menu.reload_scripts = false;
        clear_asset_caches(); // re-read the skin from disk, so edits show up
        screen->on_screen_end(L.current_screen);
        reload_current_screen_scripts();
        screen_it = L.screens.find(L.current_screen);
        screen = (screen_it != L.screens.end()) ? screen_it->second.get() : nullptr;
        if (!screen) {
            end_blend_mode();
            end_mode_2d();
            end_drawing();
            return;
        }
    }

    if (!next_screen.has_value() && debug_menu.requested_screen.has_value()) {
        clear_asset_caches(); // a debug jump re-reads the skin from disk, so edits show up
        next_screen = screen->on_screen_end(debug_menu.requested_screen.value());
        debug_menu.requested_screen.reset();
    }

    seg_mark(SEG_HOOKS);
    if (screen->screen_init) {
        PROFILE_SCOPE_N("screen draw");
        PROFILE_ZONE_TEXT(screen->screen_name);
        const auto t0 = std::chrono::steady_clock::now();
        screen->_do_draw();
        g_spike_draw_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    } else {
        g_spike_draw_ms = 0;
        if (density::freeze_handoff_active()) { // Z-DON: the old screen is gone, the next one not up yet
            density::draw_frozen(static_cast<float>(tex.screen_width), static_cast<float>(tex.screen_height));
        }
    }
    seg_mark(SEG_DRAW);
    if (L.screen_fade_start > 0.0) {
        const SkinInfo* fade_cfg = tex.get_skin_config(SC::SCREEN_FADE_MS);
        const double dur = (fade_cfg && fade_cfg->x > 0) ? fade_cfg->x : 500.0;
        const double t = (g_frame_ms - L.screen_fade_start) / dur;
        if (t >= 1.0) {
            L.screen_fade_start = 0.0;
        } else {
            const float a = static_cast<float>(1.0 - (t < 0.0 ? 0.0 : t));
            draw_rectangle(0, 0, tex.screen_width, tex.screen_height, fade(Color::BLACK, a));
        }
    }

    if (next_screen.has_value()) {
        spdlog::info("Screen changed from {} to {}", L.current_screen, next_screen.value());
        clear_input_buffers();
        if (tex.get_skin_config(SC::SCREEN_FADE_MS) && screen_fade_applies(L.current_screen, next_screen.value()) &&
            !density::freeze_handoff_active()) {
            L.screen_fade_start = g_frame_ms;
        } else {
            L.screen_fade_start = 0.0;
        }
        global_data.previous_screen = global_data.current_screen;
        L.current_screen = next_screen.value();
        global_data.current_screen = screens_to_string(L.current_screen);
        reset_input_lock();
        script_manager.refresh_method_cache(); // sol metatables pushed lazily on the last screen
        script_manager.full_collect();
    } else {
        static const bool gc_stress = std::getenv("ZDON_LUA_GC_STRESS") != nullptr;
        if (gc_stress) {
            script_manager.full_collect(); // dev hook: every frame, shakes out objects only Lua kept alive
        }
    }

    seg_mark(SEG_SWITCH);
    if (!global_data.skinless) {
        draw_touch_drum();

        if (global_data.config->general.fps_counter) {
            L.fps_counter.draw();
        }

        draw_skin_update_status();
        draw_zdon_update_notice(L.current_screen);

        if (L.toast) {
            L.toast->draw();
        }

        debug_menu.draw();
    }
    ime_ui_draw(density::to_window(L.camera));

    draw_outer_border(tex.screen_width, tex.screen_height, L.last_color);

    end_blend_mode();
    end_mode_2d();
    density::end_frame(global_data.camera.border_color); // the density target into the window
    seg_mark(SEG_OVERLAY);
    const bool dump_done = dump_screen_tick() || zdon_record::frame(global_data.current_screen);
    frame_stats_add(frame_start);
    {
        PROFILE_SCOPE_N("present");
        const auto present_start = std::chrono::steady_clock::now();
        end_drawing();
        draw_stats_add(screen->screen_name);
        if (dump_done) {
            g_dump_screen_done = true; // shut down as on a normal quit (std::exit here crashed
                                       // in static destructors with the worker threads still running)
        }

        // tear-free pacing: the extra (input) frames of a refresh are not presented
        if (!next_screen.has_value() && vblank_pacing::present_this_frame() && !zdon_record::skip_present()) {
            vblank_pacing::before_present();
            swap_screen_buffer();
        }
        frame_spike_present(present_start);
    }

    if (zdon_update_offered(L.current_screen) && key_pressed(SDL_SCANCODE_F9)) {
        if (zdon_update::launch_updater()) {
            g_quit_for_update = true;
        } else {
            zhud::toast(zhud::text("update_failed"));
        }
    }

    if (key_pressed(SDL_SCANCODE_F12)) {
        Image image = density::capture(); // the density target's full size
        if (L.current_screen == Screens::RESULT && tex.options[SCO::CROP_RESULT_SCREENSHOT]) {
            const auto image_width = static_cast<float>(image.width);
            const auto crop_height = static_cast<float>(image.height / 2);
            Image top = image_from_image(image, {0, 0, image_width, crop_height});
            unload_image(image);
            image = top;
        }
        const PlayerNum p = global_data.player_num == PlayerNum::P2 ? PlayerNum::P2 : PlayerNum::P1;
        const SessionData& sd = global_data.session_data[static_cast<int>(p)];
        const std::time_t now = std::time(nullptr);
        char date[32];
        std::strftime(date, sizeof(date), "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
        std::string name = std::string(date) + "_" + sd.song_title + "_" + std::to_string(sd.selected_difficulty);
        for (char& c : name) {
            if (c == ' ' || std::strchr("/\\:*?\"<>|", c)) {
                c = '_';
            }
        }
        fs::create_directories("Screenshots");
        const std::string path = "Screenshots/" + name + ".png";
        if (export_image(image, path.c_str())) {
            spdlog::info("Screenshot saved: {}", path);
        } else {
            spdlog::error("Screenshot failed: {}", path);
        }
        unload_image(image);
    }

    texture_lazy::warm_tick(); // Z-DON: textures the screen will draw, uploaded ahead in the frame's spare time
    tex.predecode_tick();      // Z-DON: the next screen's (start_predecode)

#if !defined(__EMSCRIPTEN__) && !defined(YATAIDON_PLATFORM_IOS)
    vblank_pacing::wait_next(); // no-op unless pacing is on (no target_fps; then target_duration is 0)
    if (L.target_duration.count() > 0 && !platform_present_paces_frames()) {
        L.next_frame_time += L.target_duration;
        auto now = std::chrono::steady_clock::now();
        if (L.next_frame_time < now) {
            L.next_frame_time = now;
        }
        auto spin_start = L.next_frame_time - std::chrono::microseconds(500);
        if (spin_start > now) {
            // the plain sleep overshoots by up to a 15.6 ms tick on Windows, which turned every other
            // frame into a ~17 ms one (frames came as ~1 ms / ~17 ms pairs at any target)
            if (!zdon_os::precise_sleep_us(
                    std::chrono::duration_cast<std::chrono::microseconds>(spin_start - now).count())) {
                std::this_thread::sleep_until(spin_start);
            }
        }
        while (std::chrono::steady_clock::now() < L.next_frame_time) {
        }
    }
#endif
}

// Dev hook: YATAIDON_DUMP_CHARA="<outdir>;<cos>|<head>+<body>;<anim>;<shots>" renders the
// character in a hidden window, one PNG per 250 ms of animation time, and exits.
// Dev hook: YATAIDON_DUMP_SCREEN="<outdir>;<t1,t2,...>" (ms after the first frame) runs the game in a
// hidden window, saves the frame at each listed time to <outdir>/<n>_<ms>.png and exits after the last.
// Combine with the normal CLI ("<song.tja> <difficulty> --auto") for the game and result screens.
// Dev hook: YATAIDON_DUMP_FRAMES="<outdir>;<f1,f2,...>" the same by frame index (0 = the first frame),
// saved as <outdir>/<f>.png; with YATAIDON_FIXED_STEP_MS two runs of an exe give the same frames.
static struct {
    bool active = false;
    bool by_frame = false;
    std::string outdir;
    std::vector<double> times;
    size_t next = 0;
    double t0 = -1;
} g_dump_screen;
static void dump_screen_init() {
    const char* spec = std::getenv("YATAIDON_DUMP_FRAMES");
    g_dump_screen.by_frame = spec && *spec;
    if (!g_dump_screen.by_frame) {
        spec = std::getenv("YATAIDON_DUMP_SCREEN");
    }
    if (!spec || !*spec) {
        return;
    }
    std::string sp(spec);
    size_t semi = sp.find(';');
    if (semi == std::string::npos) {
        spdlog::error("YATAIDON_DUMP_SCREEN / YATAIDON_DUMP_FRAMES: outdir;t1,t2,...");
        return;
    }
    g_dump_screen.outdir = sp.substr(0, semi);
    std::string ts = sp.substr(semi + 1);
    size_t st = 0;
    for (size_t i; (i = ts.find(',', st)) != std::string::npos; st = i + 1) {
        g_dump_screen.times.push_back(std::atof(ts.substr(st, i - st).c_str()));
    }
    if (st < ts.size()) {
        g_dump_screen.times.push_back(std::atof(ts.substr(st).c_str()));
    }
    if (g_dump_screen.times.empty()) {
        return;
    }
    fs::create_directories(g_dump_screen.outdir);
    g_dump_screen.active = true;
}
static void write_draw_log(const std::string& path) {
    // one row per draw call in draw order (= layer order): idx name x y w h tex_index scale center rotation lua site
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return;
    }
    int i = 0;
    for (const DrawLogEntry& e : debug_draw_log) {
        std::println(f, "{}\t{}\t{:.2f}\t{:.2f}\t{:.2f}\t{:.2f}\t{}\t{:.4f}\t{}\t{:.2f}\t{}:{}\t{}", i++, e.name,
                     e.rect.x, e.rect.y, e.rect.width, e.rect.height, e.index, e.scale, e.center ? 1 : 0, e.rotation,
                     e.lua_source, e.lua_line, e.lua_function);
    }
    std::fclose(f);
}
// call between BeginDrawing and EndDrawing; returns true when the last shot was taken
static bool dump_screen_tick() {
    if (!g_dump_screen.active) {
        return false;
    }
    static const auto start = std::chrono::steady_clock::now();
    const double ms = g_dump_screen.by_frame
                          ? static_cast<double>(g_frame_index)
                          : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // grab the frames in memory and write them after the last one: exporting a PNG takes several
    // frames and would stall the game (auto play hits once per frame)
    static std::vector<std::pair<std::string, Image>> shots;
    bool grabbed = false;
    while (g_dump_screen.next < g_dump_screen.times.size() && ms >= g_dump_screen.times[g_dump_screen.next]) {
        const std::string name =
            g_dump_screen.by_frame
                ? std::to_string(static_cast<long long>(g_dump_screen.times[g_dump_screen.next])) + ".png"
                : std::to_string(g_dump_screen.next) + "_" +
                      std::to_string(static_cast<int>(g_dump_screen.times[g_dump_screen.next])) + ".png";
        if (draw_log_dev_hook()) {
            write_draw_log((fs::path(g_dump_screen.outdir) / (name + ".draws.tsv")).string());
        }
        if (!grabbed) {
            shots.emplace_back(name, density::capture()); // the density target's full size
            grabbed = true;
        } else {
            shots.emplace_back(name, image_copy(shots.back().second));
        }
        g_dump_screen.next++;
    }
    if (g_dump_screen.next < g_dump_screen.times.size()) {
        return false;
    }
    for (auto& [name, img] : shots) {
        export_image(img, (fs::path(g_dump_screen.outdir) / name).string().c_str());
        unload_image(img);
    }
    shots.clear();
    return true;
}

static int dump_chara(const std::string& spec) {
    std::vector<std::string> f;
    size_t st = 0;
    for (size_t i; (i = spec.find(';', st)) != std::string::npos; st = i + 1) {
        f.push_back(spec.substr(st, i - st));
    }
    f.push_back(spec.substr(st));
    if (f.size() < 4) {
        spdlog::error("YATAIDON_DUMP_CHARA: outdir;model;anim;shots");
        return 1;
    }
    fs::create_directories(f[0]);
    std::unique_ptr<Chara3D> ch;
    if (auto plus = f[1].find('+'); plus != std::string::npos) {
        std::string head = f[1].substr(0, plus), body = f[1].substr(plus + 1);
        ch = std::make_unique<Chara3D>(head, body, false, true);
    } else {
        ch = std::make_unique<Chara3D>(f[1], false, true);
    }
    ch->set_don_colors({104, 191, 192, 255}, {249, 71, 40, 255}, {249, 240, 225, 255});
    ch->set_bpm(120.0f);
    ch->set_anim(static_cast<AnimIndex>(std::atoi(f[2].c_str())));
    const int shots = std::atoi(f[3].c_str());
    ray::rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD,
                                   RL_FUNC_ADD);
    double ms = 0.0;
    for (int i = 0; i < shots; i++) {
        for (int k = 0; k < 15; k++) {
            ms += 16.6667;
            ch->update(ms);
        } // ~250 ms of animation
        Camera2D cam = compute_camera2d(tex.screen_width, tex.screen_height);
        begin_drawing();
        clear_background(Color{200, 220, 200, 255});
        begin_mode_2d(cam);
        begin_blend_mode(blend_mode::CUSTOM_SEPARATE);
        ch->draw(tex.screen_width * 0.5f, tex.screen_height * 0.55f, 2.0f);
        end_blend_mode();
        end_mode_2d();
        Image img = load_image_from_screen();
        end_drawing();
        export_image(img, (fs::path(f[0]) / (std::to_string(i) + ".png")).string().c_str());
        unload_image(img);
    }
    return 0;
}

// Z-DON: with no skin yet (a first install), the system's own font for the first-run screens (language, the skin
// download): the first of these that exists, with a Korean font as the fallback for what it lacks
static void zdon_skinless_font() {
    std::vector<fs::path> fonts;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    const fs::path dir = fs::path(windir ? windir : "C:\\Windows") / "Fonts";
    for (const char* f :
         {"msjh.ttc", "msyh.ttc", "YuGothM.ttc", "meiryo.ttc", "malgun.ttf", "segoeui.ttf", "arial.ttf"}) {
        fonts.push_back(dir / f);
    }
    const fs::path korean = dir / "malgun.ttf";
#else
    for (const char* f :
         {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
          "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "/System/Library/Fonts/PingFang.ttc",
          "/System/Library/Fonts/Hiragino Sans GB.ttc", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
          "/System/Library/Fonts/Helvetica.ttc"}) {
        fonts.emplace_back(f);
    }
    const fs::path korean;
#endif
    std::error_code ec;
    for (const fs::path& f : fonts) {
        if (!fs::exists(f, ec)) {
            continue;
        }
        try {
            font_manager.init(f, f != korean && fs::exists(korean, ec) ? korean : fs::path());
            spdlog::info("Without a skin: the system font {}", f.filename().string());
            return;
        } catch (const std::exception& e) {
            spdlog::warn("Without a skin: {} ({})", f.string(), e.what());
        }
    }
    spdlog::error("Without a skin: no system font found; the first-run screens have no text");
}

static int game_main(int argc, char* argv[]) {
#ifdef _WIN32
    // The CRT's argv is in the ANSI code page: a song path with characters outside it (Japanese on a
    // Chinese-locale system) came through as '?' and was "not found". Take the arguments from the
    // wide command line as UTF-8, which path_from_arg reads.
    std::vector<std::string> utf8_args = win32_utf8_args();
    std::vector<char*> utf8_argv;
    if (!utf8_args.empty()) {
        for (std::string& a : utf8_args) {
            utf8_argv.push_back(a.data());
        }
        utf8_argv.push_back(nullptr);
        argc = static_cast<int>(utf8_args.size());
        argv = utf8_argv.data();
    }
#endif
    spdlog::info("Starting YataiDON");
    zdon_record::prepare(argc, argv); // --record: a hidden fixed-step run (environment, before anything reads it)
    set_working_directory_to_executable();
    const bool had_config = std::filesystem::exists("config.toml");
    global_data.config = new Config(get_config());
    const bool gen3 = global_data.config->general.score_method == ScoreMethod::GEN3;
    global_data.needs_onboarding = !had_config || !std::filesystem::exists(gen3 ? "scores_gen3.db" : "scores.db");

    save_config(*global_data.config);
    ensure_config_file(*global_data.config);
    Screens initial_screen = check_args(argc, argv);
    // Dev hook: YATAIDON_FORCE_AUTO=1 plays every chart on auto (the dan courses have no CLI)
    if (const char* fa = std::getenv("YATAIDON_FORCE_AUTO"); fa && *fa == '1') {
        global_data.force_auto_play = true;
    }
    init_scores_manager(global_data.config->general.score_method == ScoreMethod::GEN3);
    unsigned int flags = platform_window_flags(0);
    if (global_data.config->video.vsync && !fixed_step::on()) {
        flags |= window_flag::VSYNC;
        spdlog::info("VSync enabled");
    }
#ifdef YATAIDON_PLATFORM_IOS
    ios_configure_window_flags(flags);
#endif
    const char* dump_chara_spec = std::getenv("YATAIDON_DUMP_CHARA");
    if (dump_chara_spec && *dump_chara_spec) {
        flags |= ray::FLAG_WINDOW_HIDDEN;
    }
    dump_screen_init();
    // Dev hook: YATAIDON_TEST=1 (automated test runs) never shows its window, so it can neither take the
    // foreground nor cover the player's screen; a window created hidden still renders and reads back
    if (g_dump_screen.active || zdon_test_mode()) {
        flags |= ray::FLAG_WINDOW_HIDDEN;
    }
    set_window_flags(flags);
    quiet_raylib_log();
    setup_logging(global_data.config->general.log_level);
    if (zdon_record::coordinating()) {
        return zdon_record::coordinate(argc, argv) ? 0 : 1; // parallel recording: the parts do the work
    }
    // The library walk only reads the disk: it runs while the window opens and the skin loads, and the
    // loading screen picks it up (not with a song on the command line, which skips that screen).
    if (initial_screen == Screens::LOADING && !(dump_chara_spec && *dump_chara_spec)) {
        LoadingScreen::start_library_walk_early();
    }

    platform_init_window();
    texture_bc7::init(global_data.config->video.zdon_texture_compression);
    try {
        load_skin();
    } catch (const std::exception& e) {
        spdlog::error("Starting without a skin: {}", e.what());
        global_data.skinless = true;
        global_data.needs_onboarding = true;
        initial_screen = Screens::ONBOARDING;
        zdon_skinless_font(); // the first-run screens draw text before any skin (and its fonts) is here
    }
    if (dump_chara_spec && *dump_chara_spec) {
        std::exit(
            dump_chara(dump_chara_spec)); // exit() like the label dump: the audio thread keeps a plain return alive
    }
    if (zdon_record::active() &&
        (global_data.skinless || initial_screen == Screens::LOADING || !zdon_record::begin())) {
        spdlog::error("[record] needs a skin and a song on the command line");
        std::exit(1);
    }
    {
        const int w0 = get_render_width();
        const int h0 = get_render_height();
        // a recording takes the size of its first frame: wait until the window has taken the new size
        // (SDL reports it with an event, not on SetWindowSize's return)
        if (apply_queued_window_resize() && zdon_record::active()) {
            for (int i = 0; i < 200 && get_render_width() == w0 && get_render_height() == h0; i++) {
                poll_window_events();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    // Apply config defaults for empty access codes
    if (global_data.config->network.access_code_1.empty()) {
        global_data.config->network.access_code_1 = "0";
    }
    if (global_data.config->network.access_code_2.empty()) {
        global_data.config->network.access_code_2 = "1";
    }

    scores_manager.player_1 = global_data.config->network.access_code_1;
    scores_manager.player_2 = global_data.config->network.access_code_2;
    if (auto pd = scores_manager.get_player_data(scores_manager.player_1)) {
        scores_manager.player_1_data = *pd;
    }
    if (auto pd = scores_manager.get_player_data(scores_manager.player_2)) {
        scores_manager.player_2_data = *pd;
    }

#ifdef PLATFORM_ANDROID
    network.check_and_install_android_update();
#endif
    // YATAIDON_FIXED_STEP_MS: no skin update (a network job whose progress text shows on screen)
    if (global_data.config->general.skin_updater && !fixed_step::on()) {
        skin_updater.start();
    }
    zdon_update::start_check();

    platform_initialize_after_window();

    const int target_fps = global_data.config->video.target_fps =
        platform_apply_target_fps(global_data.config->video.target_fps);
    if (target_fps != -1) {
        spdlog::info("Target FPS set to {}", target_fps);
    }

    g_loop = new LoopState();
    LoopState& L = *g_loop;

    L.current_screen = initial_screen;
    global_data.current_screen = screens_to_string(initial_screen);
    L.target_duration = frame_duration(target_fps);

    vblank_pacing::init(global_data.config->video.vsync, target_fps);
    if (vblank_pacing::wants_vsync()) {
        ray::SetWindowState(ray::FLAG_VSYNC_HINT); // the shown frame waits for the refresh: no tear line
    }
    populate_screens(L.screens);

    L.camera = compute_camera2d(tex.screen_width, tex.screen_height);

    zdon_composited_fullscreen = global_data.config->video.zdon_composited_fullscreen;
#if !defined(__EMSCRIPTEN__) && !defined(YATAIDON_PLATFORM_IOS)
    if ((flags & ray::FLAG_WINDOW_HIDDEN) &&
        (global_data.config->video.borderless || global_data.config->video.fullscreen)) {
        // a hidden window does not go fullscreen (SDL keeps it pending until shown): give it the monitor's size
        // without a frame instead, so it renders the frames a fullscreen window would
        ray::SetWindowState(ray::FLAG_WINDOW_UNDECORATED);
        const int monitor = ray::GetCurrentMonitor();
        set_window_size(ray::GetMonitorWidth(monitor), ray::GetMonitorHeight(monitor));
        spdlog::info("Hidden window sized to the monitor ({}x{}) instead of fullscreen", ray::GetMonitorWidth(monitor),
                     ray::GetMonitorHeight(monitor));
    } else {
        if (global_data.config->video.borderless) {
            toggle_borderless_windowed();
            spdlog::info("Borderless window enabled");
        }
        if (global_data.config->video.fullscreen) {
            toggle_fullscreen();
            spdlog::info("Fullscreen enabled");
        }
    }
#endif

    set_separate_alpha_blend();
    disable_exit_key();
    hide_cursor();

    L.next_frame_time = std::chrono::steady_clock::now();
#ifdef __EMSCRIPTEN__
    return emscripten_run_main_loop(run_frame);
#elif defined(YATAIDON_PLATFORM_IOS)
    return ios_run_main_loop(run_frame);
#else
#ifdef YATAIDON_INPUT_THREAD
    input_thread = std::thread(input_polling_thread);
#endif
    start_midi_input(global_data.config->midi);

    while (!window_should_close() && !g_dump_screen_done && !g_quit_for_update) {
        process_midi_events();

        // Consume buffered quit-key presses while editing to prevent a delayed game exit after the field closes.
        if (check_key_pressed(global_data.config->keys.exit_key) &&
            !is_text_input_key(global_data.config->keys.exit_key)) {
            break;
        }
        run_frame();
    }

    shutdown_midi_input();
    input_thread_running = false;
    if (input_thread.joinable()) {
        input_thread.join();
    }
    network.shutdown();
    skin_updater.shutdown();
    zdon_update::shutdown();
    shutdown_sdl_joysticks();
    delete g_loop;
    g_loop = nullptr;
    const bool record_ok = zdon_record::end(); // encodes the rest and muxes the audio in
    zdon_record::export_cancel();              // the recording mode's recording, if the game quit during it
    get_song_library_loader().stop();
    // Cleanup needs a live SDL/OpenGL context and config.
    webcam.close();
    unload_skin();
    texture_bc7::shutdown();
    delete global_data.config;
    global_data.config = nullptr;
    platform_close_window();
    spdlog::info("Game closed");
    return record_ok ? 0 : 1;
#endif
    return 0;
}

int main(int argc, char* argv[]) {
    return platform_run_main(game_main, argc, argv);
}
