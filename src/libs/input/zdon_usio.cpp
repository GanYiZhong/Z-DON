#include "zdon_usio.h"

#include <atomic>
#include <spdlog/spdlog.h>

namespace zdon_usio {
    namespace {
        std::atomic<int> pending_coins{0};
        std::atomic<int> pending_service{0};
        std::atomic<int> pending_test{0};
        std::mutex state_mutex;
        Snapshot state; // state_mutex
    } // namespace

    Snapshot snapshot() {
        std::scoped_lock lock(state_mutex);
        Snapshot out = state;
        state.max_since = state.value;
        return out;
    }

    int take_coins() {
        return pending_coins.exchange(0, std::memory_order_acq_rel);
    }
    int take_service() {
        return pending_service.exchange(0, std::memory_order_acq_rel);
    }
    int take_test_presses() {
        return pending_test.exchange(0, std::memory_order_acq_rel);
    }
} // namespace zdon_usio

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <windows.h>

#include "../zdon_os.h"

namespace zdon_usio {

    namespace {
        // bnusio.dll exports, as TaikoArcadeLoader declares them (x64: one calling convention)
        using OpenFn = int64_t (*)();
        using CloseFn = int64_t (*)();
        using CommunicationFn = uint64_t (*)(int32_t);
        using IsConnectedFn = uint8_t (*)();
        using GetAnalogInFn = uint16_t (*)(uint8_t);
        using GetSwInFn = uint32_t (*)();
        using GetCounterFn = uint16_t (*)(int32_t); // bnusio_GetCoin / bnusio_GetService: running totals
        using GetIoBoardNameFn = char* (*)();
        using GetFirmwareVersionFn = size_t (*)();

        std::atomic<bool> run{false};
        std::thread worker;
        HMODULE library = nullptr;

        std::filesystem::path exe_dir() {
            std::wstring buf(MAX_PATH, L'\0');
            for (;;) {
                const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
                if (n == 0) {
                    return std::filesystem::current_path();
                }
                if (n < buf.size()) {
                    buf.resize(n);
                    break;
                }
                buf.resize(buf.size() * 2);
            }
            return std::filesystem::path(buf).parent_path();
        }

        template<class T>
        T proc(HMODULE lib, const char* name) {
            return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(lib, name)));
        }

        // A running total from the board: the first reading is the base, later ones add what they grew by
        // (a 16-bit total wraps back to 0)
        struct Counter {
            bool seen = false;
            uint16_t last = 0;
            int step(uint16_t now) {
                if (!seen) {
                    seen = true;
                    last = now;
                    return 0;
                }
                const auto grew = static_cast<uint16_t>(now - last);
                last = now;
                return grew < 100 ? grew : 0; // a reset board reads lower: not thousands of coins
            }
        };

        // A hit is the stronger sensor of one side of one drum once it reaches the threshold; that side then rests
        // for holdoff_ms (a piezo rings on after the stick leaves it) and must fall back below the threshold
        // before it counts again. A don hit also shakes the rim sensor, which the stronger-sensor pick absorbs.
        void loop(HMODULE lib, Settings s, Sinks sinks) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            auto open = proc<OpenFn>(lib, "bnusio_Open");
            auto close = proc<CloseFn>(lib, "bnusio_Close");
            auto communication = proc<CommunicationFn>(lib, "bnusio_Communication");
            auto is_connected = proc<IsConnectedFn>(lib, "bnusio_IsConnected");
            auto analog = proc<GetAnalogInFn>(lib, "bnusio_GetAnalogIn");
            auto sw_in = proc<GetSwInFn>(lib, "bnusio_GetSwIn");
            auto get_coin = proc<GetCounterFn>(lib, "bnusio_GetCoin");
            auto get_service = proc<GetCounterFn>(lib, "bnusio_GetService");
            auto board_name = proc<GetIoBoardNameFn>(lib, "bnusio_GetIoBoardName");
            auto firmware = proc<GetFirmwareVersionFn>(lib, "bnusio_GetFirmwareVersion");

            const int64_t opened = open();
            spdlog::info("USIO: bnusio_Open returned {}", opened);
            if (communication) {
                communication(0);
            }
            if (is_connected) {
                spdlog::info("USIO: connected = {}", static_cast<int>(is_connected()));
            }
            if (board_name) {
                const char* name = board_name();
                spdlog::info("USIO: board '{}'", name ? name : "");
            }
            if (firmware) {
                spdlog::info("USIO: firmware {}", firmware());
            }
            {
                std::scoped_lock lock(state_mutex);
                state = Snapshot{};
                state.active = true;
                state.connected = is_connected ? is_connected() != 0 : true;
                if (board_name) {
                    const char* name = board_name();
                    state.board = name ? name : "";
                }
                state.firmware = firmware ? static_cast<uint64_t>(firmware()) : 0;
                state.threshold = s.threshold;
                state.has_switches = sw_in != nullptr;
            }
            spdlog::info("USIO: switches {}, coin counter {}, service counter {}", sw_in ? "yes" : "no",
                         get_coin ? "yes" : "no", get_service ? "yes" : "no");

            using clock = std::chrono::steady_clock;
            // [player][side]: side 0 left, 1 right
            std::array<std::array<clock::time_point, 2>, 2> rest_until{};
            std::array<std::array<bool, 2>, 2> armed{{{true, true}, {true, true}}};
            std::array<uint16_t, 8> peak{};
            uint32_t prev_sw = 0;
            bool sw_seen = false;
            Counter coins;
            Counter service;
            const auto started = clock::now();
            auto last_report = started;
            long long reads = 0;
            bool rate_logged = false;
            std::array<int, 8> hits{};
            std::array<uint16_t, 8> last_hit{};
            uint32_t sw_now = 0;
            int coin_total = -1;
            int service_total = -1;
            auto rate_since = started;
            long long rate_reads = 0;
            double reads_per_sec = 0;
            auto connected_checked = started;
            bool connected_now = is_connected ? is_connected() != 0 : true;
            const auto holdoff = std::chrono::milliseconds(s.holdoff_ms);
            const int threshold = s.threshold;

            while (run.load(std::memory_order_acquire)) {
                if (communication) {
                    communication(0);
                }
                std::array<uint16_t, 8> v{};
                for (uint8_t ch = 0; ch < 8; ch++) {
                    v[ch] = analog(ch);
                    peak[ch] = std::max(peak[ch], v[ch]);
                }
                const auto now = clock::now();
                reads++;
                for (int p = 0; p < 2; p++) {
                    for (int side = 0; side < 2; side++) {
                        // channels per player: left kat, left don, right don, right kat
                        const int kat_ch = (p * 4) + (side == 0 ? 0 : 3);
                        const int don_ch = (p * 4) + (side == 0 ? 1 : 2);
                        const int kat = v[kat_ch];
                        const int don = v[don_ch];
                        const int top = std::max(kat, don);
                        if (top < threshold) {
                            armed[p][side] = true;
                            continue;
                        }
                        if (!armed[p][side] || now < rest_until[p][side]) {
                            continue;
                        }
                        armed[p][side] = false;
                        rest_until[p][side] = now + holdoff;
                        const bool is_don = don >= kat;
                        const int won_ch = is_don ? don_ch : kat_ch;
                        hits[won_ch]++;
                        last_hit[won_ch] = static_cast<uint16_t>(top);
                        const int pad = is_don ? (side == 0 ? 0 : 1) : (side == 0 ? 2 : 3);
                        const float strength = std::clamp(static_cast<float>(top) / 16384.0f, 0.5f, 1.15f);
                        spdlog::debug("USIO: P{} {} {} value {} (other {})", p + 1, side == 0 ? "left" : "right",
                                      is_don ? "don" : "kat", top, is_don ? kat : don);
                        if (sinks.hit) {
                            sinks.hit(p, pad, strength);
                        }
                    }
                }

                if (sw_in) {
                    const uint32_t sw = sw_in();
                    sw_now = sw;
                    // a switch already down when the game starts (TEST left on) is not a press
                    const uint32_t pressed = sw_seen ? (sw & ~prev_sw) : 0;
                    prev_sw = sw;
                    sw_seen = true;
                    if (pressed != 0) {
                        spdlog::debug("USIO: switches {:#x} (new {:#x})", sw, pressed);
                    }
                    if ((pressed & SW_TEST) != 0) {
                        pending_test.fetch_add(1, std::memory_order_acq_rel);
                    }
                    if (sinks.menu) {
                        if ((pressed & SW_UP) != 0) {
                            sinks.menu(-1);
                        }
                        if ((pressed & SW_DOWN) != 0) {
                            sinks.menu(1);
                        }
                        if ((pressed & SW_ENTER) != 0) {
                            sinks.menu(0);
                        }
                    }
                }
                if (get_coin) {
                    const uint16_t total = get_coin(0);
                    coin_total = total;
                    if (const int n = coins.step(total); n > 0) {
                        spdlog::info("USIO: {} coin(s)", n);
                        pending_coins.fetch_add(n, std::memory_order_acq_rel);
                    }
                }
                if (get_service) {
                    const uint16_t total = get_service(0);
                    service_total = total;
                    if (const int n = service.step(total); n > 0) {
                        spdlog::info("USIO: {} service credit(s)", n);
                        pending_service.fetch_add(n, std::memory_order_acq_rel);
                    }
                }

                rate_reads++;
                // the board's link, again every second (the test screen shows it)
                if (is_connected && now - connected_checked >= std::chrono::seconds(1)) {
                    connected_checked = now;
                    connected_now = is_connected() != 0;
                }
                if (now - rate_since >= std::chrono::milliseconds(250)) {
                    reads_per_sec =
                        static_cast<double>(rate_reads) / std::chrono::duration<double>(now - rate_since).count();
                    rate_reads = 0;
                    rate_since = now;
                }
                {
                    std::scoped_lock lock(state_mutex);
                    state.value = v;
                    for (size_t ch = 0; ch < v.size(); ch++) {
                        state.max_since[ch] = std::max(state.max_since[ch], v[ch]);
                    }
                    state.hits = hits;
                    state.last_hit = last_hit;
                    state.switches = sw_now;
                    state.coin_total = coin_total;
                    state.service_total = service_total;
                    state.reads_per_sec = reads_per_sec;
                    state.connected = connected_now;
                }

                if (!rate_logged && now - started > std::chrono::seconds(3)) {
                    rate_logged = true;
                    const double secs = std::chrono::duration<double>(now - started).count();
                    spdlog::info("USIO: {:.0f} reads/s ({:.0f} us each)", reads / secs, secs * 1e6 / reads);
                }
                // the loudest reading per sensor every few seconds at debug level, to set the threshold by
                if (now - last_report > std::chrono::seconds(5)) {
                    if (std::ranges::any_of(peak, [](uint16_t x) { return x != 0; })) {
                        spdlog::debug("USIO: peaks P1 {} {} {} {} P2 {} {} {} {}", peak[0], peak[1], peak[2], peak[3],
                                      peak[4], peak[5], peak[6], peak[7]);
                    }
                    peak.fill(0);
                    last_report = now;
                }
                // Windows timers wake no sooner than ~1 ms: a shorter wait spins on the clock (one core kept busy)
                if (s.poll_us >= 1000) {
                    if (!zdon_os::precise_sleep_us(s.poll_us)) {
                        std::this_thread::sleep_for(std::chrono::microseconds(s.poll_us));
                    }
                } else if (s.poll_us > 0) {
                    const auto until = now + std::chrono::microseconds(s.poll_us);
                    while (clock::now() < until) {
                        YieldProcessor();
                    }
                }
            }
            if (close) {
                close();
            }
            std::scoped_lock lock(state_mutex);
            state.active = false;
        }
    } // namespace

    void start(const Settings& settings, Sinks sinks) {
        if (run.load()) {
            return;
        }
        std::filesystem::path path = std::filesystem::path(settings.dll).make_preferred();
        if (path.is_relative()) {
            path = exe_dir() / path;
        }
        // its own folder first, so the DLLs bnusio.dll needs next to it load too
        library = LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!library) {
            spdlog::error("USIO: cannot load '{}' (error {})", path.string(), GetLastError());
            return;
        }
        if (!proc<OpenFn>(library, "bnusio_Open") || !proc<GetAnalogInFn>(library, "bnusio_GetAnalogIn")) {
            spdlog::error("USIO: '{}' has no bnusio_Open / bnusio_GetAnalogIn", path.string());
            FreeLibrary(library);
            library = nullptr;
            return;
        }
        spdlog::info("USIO: using '{}' (threshold {}, hold-off {} ms, poll {} us)", path.string(), settings.threshold,
                     settings.holdoff_ms, settings.poll_us);
        run.store(true, std::memory_order_release);
        worker = std::thread(loop, library, settings, std::move(sinks));
    }

    void stop() {
        run.store(false, std::memory_order_release);
        if (worker.joinable()) {
            worker.join();
        }
        if (library) {
            FreeLibrary(library);
            library = nullptr;
        }
    }

    bool running() {
        return run.load(std::memory_order_acquire);
    }

} // namespace zdon_usio

#else

namespace zdon_usio {

    void start(const Settings& /*settings*/, Sinks /*sinks*/) {
        spdlog::warn("USIO: only supported on Windows");
    }
    void stop() {}
    bool running() {
        return false;
    }

} // namespace zdon_usio

#endif
