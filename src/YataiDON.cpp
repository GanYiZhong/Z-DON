#include <cstdlib>
#include <iostream>
#include <rlgl.h>

#include "libs/animation.h"
#include "libs/audio.h"
#include "libs/camera_utils.h"
#include "libs/filesystem.h"
#include "libs/global_data.h"
#include "libs/input.h"
#include "libs/logging.h"
#include "libs/network.h"
#include "libs/profiler.h"
#include "libs/screen.h"
#include "libs/script.h"
#include "libs/skin_updater.h"
#include "libs/song_parser.h"
#include "libs/test_mode.h"
#include "libs/text.h"
#include "libs/webcam.h"
#include "objects/global/chara_3d.h"
#include "platform/vblank_pacing.h"

#ifdef _WIN32
#include "platform/platform_windows.h"
#elif defined(__ANDROID__)
#include "platform/platform_android.h"
#elif defined(__EMSCRIPTEN__)
#include "platform/platform_emscripten.h"
#elif defined(__linux__)
#include "platform/platform_linux.h"
#endif

#ifdef PLATFORM_ANDROID
#include <SDL3/SDL_main.h>
#endif

#include "objects/global/debug_menu.h"
#include "objects/global/fps_counter.h"
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
#include "scenes/loading.h"
#include "scenes/result.h"
#include "scenes/result_2p.h"
#include "scenes/retry_game.h"
#include "scenes/settings.h"
#include "scenes/song_select.h"
#include "scenes/song_select_2p.h"
#include "scenes/song_select_practice.h"
#include "scenes/title.h"

void draw_outer_border(int screen_width, int screen_height, ray::Color last_color) {
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx != 0 || dy != 0) {
                DrawRectangle(dx * screen_width, dy * screen_height, screen_width, screen_height, last_color);
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

    const std::string skin_line =
        st.current_skin.empty() ? "Checking for skin updates..." : "Updating skin: " + st.current_skin;
    ray::Font skin_font = font_manager.get_font(skin_line, size);
    ray::DrawTextEx(skin_font, skin_line.c_str(), ray::Vector2{x, y}, static_cast<float>(size), 1.0f, ray::YELLOW);

    if (!st.current_file.empty()) {
        y += static_cast<float>(size);
        ray::Font file_font = font_manager.get_font(st.current_file, size);
        ray::DrawTextEx(file_font, st.current_file.c_str(), ray::Vector2{x, y}, static_cast<float>(size), 1.0f,
                        ray::WHITE);
    }
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

// Consumes --card-port/--card-baud/--card-poll-ms (and removes them from argv) so check_args never sees them.
static void parse_card_reader_args(int& argc, char* argv[], CardReaderConfig& cr) {
    int out = 1;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        const bool known = arg == "--card-port" || arg == "--card-baud" || arg == "--card-poll-ms";
        if (!known) {
            argv[out++] = argv[i];
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Error: " << arg << " requires a value\n";
            exit_now(1);
        }
        std::string val = argv[++i];
        if (arg == "--card-port") {
            cr.port = val;
            continue;
        }
        try {
            size_t pos = 0;
            int n = std::stoi(val, &pos);
            if (pos != val.size() || n <= 0) {
                throw std::invalid_argument(val);
            }
            (arg == "--card-baud" ? cr.baudrate : cr.poll_interval_ms) = n;
        } catch (const std::exception&) {
            std::cerr << "Error: Invalid value for " << arg << ": " << val << "\n";
            exit_now(1);
        }
    }
    argc = out;
}

Screens check_args(int argc, char* argv[]) {
    if (argc == 1) {
        return Screens::LOADING;
    }

    std::string song_path;
    std::optional<int> difficulty;
    bool auto_play = false;
    bool practice = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--auto") {
            auto_play = true;
        } else if (arg == "--practice") {
            practice = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " <song_path> [difficulty] [--auto] [--practice]\n";
            std::cout << "  song_path   : Path to the TJA song file\n";
            std::cout << "  difficulty  : Difficulty level (optional, defaults to max difficulty)\n";
            std::cout << "  --auto      : Enable auto mode\n";
            std::cout << "  --practice  : Start in practice mode\n";
            std::cout << "Card reader (valid with or without song_path):\n";
            std::cout << "  --card-port <path>   : Serial port (default /dev/ttyUSB0)\n";
            std::cout << "  --card-baud <rate>   : Baud rate (default 38400)\n";
            std::cout << "  --card-poll-ms <ms>  : Poll interval (default 100)\n";
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
    SongParser tja = [&]() -> SongParser {
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
        if (course_data.find(difficulty.value()) == course_data.end()) {
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

    Screens current_screen = practice ? Screens::GAME_PRACTICE : Screens::GAME;
    global_data.session_data[(int)PlayerNum::P1].selected_song = path;
    global_data.session_data[(int)PlayerNum::P1].selected_difficulty = selected_difficulty;
    if (auto_play) {
        global_data.force_auto_play = true;
    }

    return current_screen;
}

struct LoopState {
    std::unordered_map<Screens, std::unique_ptr<Screen>> screens;
    Screens current_screen = Screens::LOADING;
    ray::Camera2D camera = {};
    std::chrono::steady_clock::duration target_duration =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / 60.0));
    std::chrono::time_point<std::chrono::steady_clock> next_frame_time = std::chrono::steady_clock::now();
    FPSCounter fps_counter;
    ray::Color last_color = ray::BLACK;
    double screen_fade_start = 0.0;
};

static bool screen_fade_applies(Screens from, Screens to) {
    switch (to) {
    case Screens::GAME:
    case Screens::GAME_2P:
    case Screens::GAME_DAN:
    case Screens::GAME_PRACTICE:
    case Screens::AI_GAME:
    case Screens::RESULT:
    case Screens::RESULT_2P:
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
    put_screen<LoadingScreen>(screens, except, Screens::LOADING);
    put_screen<GameScreen>(screens, except, Screens::GAME);
    put_screen<Game2PScreen>(screens, except, Screens::GAME_2P);
    put_screen<PracticeGameScreen>(screens, except, Screens::GAME_PRACTICE);
    put_screen<PracticeSongSelectScreen>(screens, except, Screens::PRACTICE_SELECT);
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
}

void drop_other_screens_for_skin_reload() {
    if (!g_loop) {
        return;
    }
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

static bool dump_screen_tick();         // dev hook, defined below
static bool g_dump_screen_done = false; // the dump hook's last shot is taken: leave the main loop

// Dev hook: YATAIDON_FRAME_STATS=1 logs, every 2 s, the CPU time each frame spent before present
// (input, update, draw submission; not the vsync / frame-limiter wait).
static void frame_stats_add(std::chrono::steady_clock::time_point start) {
    static const bool on = [] {
        const char* v = std::getenv("YATAIDON_FRAME_STATS");
        return v && *v == '1';
    }();
    if (!on) {
        return;
    }
    static std::vector<double> work;
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    work.push_back(std::chrono::duration<double, std::milli>(now - start).count());
    if (now - window >= std::chrono::seconds(2)) {
        std::sort(work.begin(), work.end());
        double sum = 0;
        for (double v : work) {
            sum += v;
        }
        spdlog::info("[frame] {} frames, work ms: avg {:.3f} p50 {:.3f} p99 {:.3f} max {:.3f}", work.size(),
                     sum / work.size(), work[work.size() / 2], work[work.size() * 99 / 100], work.back());
        work.clear();
        window = now;
    }
}

static void run_frame() {
    PROFILE_FRAME();
    PROFILE_SCOPE();
    LoopState& L = *g_loop;
    const auto frame_start = std::chrono::steady_clock::now();

    g_frame_ms = vblank_pacing::frame_ms(get_current_ms());

    ray::PollInputEvents();
#if defined(YATAIDON_PLATFORM_IOS)
    if (ios_is_suspended_state()) {
        return;
    }
#endif
#if defined(YATAIDON_PLATFORM_IOS) || defined(__EMSCRIPTEN__)
    poll_keyboard_once(); // no input thread on these platforms
#endif
    poll_touch_once();

    apply_queued_window_resize();

    if (check_key_pressed(global_data.config->keys.fullscreen_key)) {
        ray::ToggleFullscreen();
        spdlog::info("Toggled fullscreen");
    } else if (check_key_pressed(global_data.config->keys.borderless_key)) {
        ray::ToggleBorderlessWindowed();
        spdlog::info("Toggled borderless windowed mode");
    }

    L.camera = compute_camera2d(tex.screen_width, tex.screen_height);
    debug_menu.update(L.camera);

    ray::BeginDrawing();

    if (global_data.camera.border_color != L.last_color) {
        ray::ClearBackground(global_data.camera.border_color);
        L.last_color = global_data.camera.border_color;
    }

    ray::BeginMode2D(L.camera);
    ray::BeginBlendMode(ray::BLEND_CUSTOM_SEPARATE);

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
        ray::EndBlendMode();
        ray::EndMode2D();
        ray::EndDrawing();
        return;
    }

    network.update(g_frame_ms);
    std::optional<Screens> next_screen;
    {
        PROFILE_SCOPE_N("screen update");
        PROFILE_ZONE_TEXT(screen->screen_name);
        next_screen = screen->update();
    }
    sync_text_input();

    // Dev hook: YATAIDON_JUMP_SCREEN="<entry|song_select|dan_select|title>;<ms>" jumps there once,
    // <ms> after the first frame (with YATAIDON_DUMP_SCREEN: screens the CLI can't start in)
    {
        static const auto jump_start = std::chrono::steady_clock::now();
        static bool jump_done = false;
        static const char* jump = std::getenv("YATAIDON_JUMP_SCREEN");
        if (!jump_done && jump && *jump) {
            std::string j(jump);
            size_t semi = j.find(';');
            double at = semi == std::string::npos ? 3000.0 : std::atof(j.substr(semi + 1).c_str());
            std::string name = j.substr(0, semi);
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - jump_start).count() >=
                at) {
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
        const double since =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - jump_start).count();
        for (auto it = fake_keys.begin(); it != fake_keys.end();) {
            if (since >= it->first) {
                spdlog::info("[devkeys] key {} at {:.0f} ms", it->second, since);
                dev_inject_key(it->second);
                it = fake_keys.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!next_screen.has_value() && debug_menu.requested_screen.has_value()) {
        next_screen = screen->on_screen_end(debug_menu.requested_screen.value());
        debug_menu.requested_screen.reset();
    }

    if (screen->screen_init) {
        PROFILE_SCOPE_N("screen draw");
        PROFILE_ZONE_TEXT(screen->screen_name);
        screen->_do_draw();
    }
    if (L.screen_fade_start > 0.0) {
        const SkinInfo* fade_cfg = tex.get_skin_config(SC::SCREEN_FADE_MS);
        const double dur = (fade_cfg && fade_cfg->x > 0) ? fade_cfg->x : 500.0;
        const double t = (g_frame_ms - L.screen_fade_start) / dur;
        if (t >= 1.0) {
            L.screen_fade_start = 0.0;
        } else {
            const float a = static_cast<float>(1.0 - (t < 0.0 ? 0.0 : t));
            ray::DrawRectangle(0, 0, tex.screen_width, tex.screen_height, ray::Fade(ray::BLACK, a));
        }
    }

    if (next_screen.has_value()) {
        spdlog::info("Screen changed from {} to {}", L.current_screen, next_screen.value());
        clear_input_buffers();
        if (tex.get_skin_config(SC::SCREEN_FADE_MS) && screen_fade_applies(L.current_screen, next_screen.value())) {
            L.screen_fade_start = g_frame_ms;
        } else {
            L.screen_fade_start = 0.0;
        }
        global_data.previous_screen = global_data.current_screen;
        L.current_screen = next_screen.value();
        global_data.current_screen = screens_to_string(L.current_screen);
        reset_input_lock();
        script_manager.refresh_method_cache(); // sol metatables pushed lazily on the last screen
    }

    draw_touch_drum();

    if (global_data.config->general.fps_counter) {
        L.fps_counter.update();
        L.fps_counter.draw();
    }

    draw_skin_update_status();

    debug_menu.draw();

    draw_outer_border(tex.screen_width, tex.screen_height, L.last_color);

    ray::EndBlendMode();
    ray::EndMode2D();
    const bool dump_done = dump_screen_tick();
    frame_stats_add(frame_start);
    {
        PROFILE_SCOPE_N("present");
        ray::EndDrawing();
        if (dump_done) {
            g_dump_screen_done = true; // shut down as on a normal quit (std::exit here crashed
                                       // in static destructors with the worker threads still running)
        }

        if (!next_screen.has_value()) {
            ray::SwapScreenBuffer();
        }
    }

    if (ray::IsKeyPressed(ray::KEY_F12)) {
        static int screenshot_counter = 0;
        ray::Image image = ray::LoadImageFromScreen();
        if (L.current_screen == Screens::RESULT && tex.options[SCO::CROP_RESULT_SCREENSHOT]) {
            ray::ImageCrop(&image, {0, 0, (float)image.width, (float)image.height / 2.0f});
        }
        ray::ExportImage(image, ray::TextFormat("screenshot%03i.png", screenshot_counter));
        ray::UnloadImage(image);
        screenshot_counter++;
        spdlog::info("Screenshot saved");
    }

#if !defined(__EMSCRIPTEN__) && !defined(YATAIDON_PLATFORM_IOS)
    vblank_pacing::wait_next(); // no-op unless uncapped without vsync (then target_duration is 0)
    if (L.target_duration.count() > 0) {
        L.next_frame_time += L.target_duration;
        auto now = std::chrono::steady_clock::now();
        if (L.next_frame_time < now) {
            L.next_frame_time = now;
        }
        auto spin_start = L.next_frame_time - std::chrono::microseconds(500);
        if (spin_start > now) {
#ifdef _WIN32
            // the plain sleep overshoots by up to a 15.6 ms tick, which turned every other frame
            // into a ~17 ms one (frames came as ~1 ms / ~17 ms pairs at any target)
            if (!win32_precise_sleep_us(
                    std::chrono::duration_cast<std::chrono::microseconds>(spin_start - now).count()))
#endif
                std::this_thread::sleep_until(spin_start);
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
static struct {
    bool active = false;
    std::string outdir;
    std::vector<double> times;
    size_t next = 0;
    double t0 = -1;
} g_dump_screen;
static void dump_screen_init() {
    const char* spec = std::getenv("YATAIDON_DUMP_SCREEN");
    if (!spec || !*spec) {
        return;
    }
    std::string sp(spec);
    size_t semi = sp.find(';');
    if (semi == std::string::npos) {
        spdlog::error("YATAIDON_DUMP_SCREEN: outdir;t1,t2,...");
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
// call between BeginDrawing and EndDrawing; returns true when the last shot was taken
static bool dump_screen_tick() {
    if (!g_dump_screen.active) {
        return false;
    }
    static const auto start = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // grab the frames in memory and write them after the last one: exporting a PNG takes several
    // frames and would stall the game (auto play hits once per frame)
    static std::vector<std::pair<std::string, ray::Image>> shots;
    bool grabbed = false;
    while (g_dump_screen.next < g_dump_screen.times.size() && ms >= g_dump_screen.times[g_dump_screen.next]) {
        const std::string name = std::to_string(g_dump_screen.next) + "_" +
                                 std::to_string((int)g_dump_screen.times[g_dump_screen.next]) + ".png";
        if (!grabbed) {
            shots.emplace_back(name, ray::LoadImageFromScreen());
            grabbed = true;
        } else {
            shots.emplace_back(name, ray::ImageCopy(shots.back().second));
        }
        g_dump_screen.next++;
    }
    if (g_dump_screen.next < g_dump_screen.times.size()) {
        return false;
    }
    for (auto& [name, img] : shots) {
        ray::ExportImage(img, (fs::path(g_dump_screen.outdir) / name).string().c_str());
        ray::UnloadImage(img);
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
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD,
                              RL_FUNC_ADD);
    double ms = 0.0;
    for (int i = 0; i < shots; i++) {
        for (int k = 0; k < 15; k++) {
            ms += 16.6667;
            ch->update(ms);
        } // ~250 ms of animation
        ray::Camera2D cam = compute_camera2d(tex.screen_width, tex.screen_height);
        ray::BeginDrawing();
        ray::ClearBackground({200, 220, 200, 255});
        ray::BeginMode2D(cam);
        ray::BeginBlendMode(ray::BLEND_CUSTOM_SEPARATE);
        ch->draw(tex.screen_width * 0.5f, tex.screen_height * 0.55f, 2.0f);
        ray::EndBlendMode();
        ray::EndMode2D();
        ray::Image img = ray::LoadImageFromScreen();
        ray::EndDrawing();
        ray::ExportImage(img, (fs::path(f[0]) / (std::to_string(i) + ".png")).string().c_str());
        ray::UnloadImage(img);
    }
    return 0;
}

int main(int argc, char* argv[]) {
    spdlog::info("Starting YataiDON");
    set_working_directory_to_executable();
    global_data.config = new Config(get_config());
    // Save config after loading in case migration occurred (old fields -> new fields)
    save_config(*global_data.config);
    ensure_config_file(*global_data.config);
    parse_card_reader_args(argc, argv, global_data.config->card_reader);
    Screens initial_screen = check_args(argc, argv);
    // Dev hook: YATAIDON_FORCE_AUTO=1 plays every chart on auto (the dan courses have no CLI)
    if (const char* fa = std::getenv("YATAIDON_FORCE_AUTO"); fa && *fa == '1') {
        global_data.force_auto_play = true;
    }
    init_scores_manager(global_data.config->general.score_method == ScoreMethod::GEN3);
    unsigned int flags = ray::FLAG_WINDOW_RESIZABLE;
    if (global_data.config->video.vsync) {
        flags |= ray::FLAG_VSYNC_HINT;
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
    if (g_dump_screen.active) {
        flags |= ray::FLAG_WINDOW_HIDDEN;
    }
    ray::SetConfigFlags(flags);
    ray::SetTraceLogLevel(ray::LOG_ERROR);
    setup_logging(global_data.config->general.log_level);

    ray::InitWindow(1280, 720, "YataiDON");
    // Dev hook: YATAIDON_TEST=1 (automated test runs) starts minimized, out of the player's way.
    // Not when the window is hidden (YATAIDON_DUMP_SCREEN): minimizing a hidden window stalled the
    // main loop for over a minute.
    if (zdon_test_mode() && !(flags & ray::FLAG_WINDOW_HIDDEN)) {
        ray::MinimizeWindow();
    }
    load_skin();
    if (dump_chara_spec && *dump_chara_spec) {
        std::exit(
            dump_chara(dump_chara_spec)); // exit() like the label dump: the audio thread keeps a plain return alive
    }
    apply_queued_window_resize();

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
    if (global_data.config->general.skin_updater) {
        skin_updater.start();
    }

#ifdef YATAIDON_PLATFORM_IOS
    ios_initialize_after_window();
#endif

    double target_fps = global_data.config->video.target_fps;
    if (target_fps != -1) {
        spdlog::info("Target FPS set to {}", target_fps);
    }

    g_loop = new LoopState();
    LoopState& L = *g_loop;

    L.current_screen = initial_screen;
    global_data.current_screen = screens_to_string(initial_screen);
    L.target_duration = (target_fps > 0.0) ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                 std::chrono::duration<double>(1.0 / target_fps))
                                           : std::chrono::steady_clock::duration::zero();

    vblank_pacing::init(global_data.config->video.vsync, target_fps);
    populate_screens(L.screens);

    L.camera = compute_camera2d(tex.screen_width, tex.screen_height);

#if !defined(__EMSCRIPTEN__) && !defined(YATAIDON_PLATFORM_IOS)
    if (global_data.config->video.borderless) {
        ray::ToggleBorderlessWindowed();
        spdlog::info("Borderless window enabled");
    }
    if (global_data.config->video.fullscreen) {
        ray::ToggleFullscreen();
        spdlog::info("Fullscreen enabled");
    }
#endif

    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD,
                              RL_FUNC_ADD);
    ray::SetExitKey(ray::KEY_NULL);
    ray::HideCursor();

    L.next_frame_time = std::chrono::steady_clock::now();
#ifdef __EMSCRIPTEN__
    return emscripten_run_main_loop(run_frame);
#elif defined(YATAIDON_PLATFORM_IOS)
    return ios_run_main_loop(run_frame);
#else
    input_thread = std::thread(input_polling_thread);
    start_midi_input(global_data.config->midi);

    while (!ray::WindowShouldClose() && !g_dump_screen_done) {
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
    shutdown_sdl_joysticks();
    delete g_loop;
    g_loop = nullptr;
    // Cleanup needs a live SDL/OpenGL context and config.
    webcam.close();
    unload_skin();
    delete global_data.config;
    global_data.config = nullptr;
#if defined(__linux__) && !defined(__ANDROID__)
    unix_close_window();
#else
    ray::CloseWindow();
#endif
    spdlog::info("Game closed");
#endif
}
