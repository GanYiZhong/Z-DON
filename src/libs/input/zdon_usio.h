#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

// Z-DON: drums and cabinet buttons on a cabinet's USIO I/O board, read through the cabinet's own bnusio.dll (not
// shipped with Z-DON; the player points [usio] dll at theirs). Windows only; elsewhere start() logs and does nothing.
// The board gives eight analog sensor values (per player left kat, left don, right don, right kat), a switch word
// (TEST, SERVICE, the test menu's up / down / enter) and coin and service counters.
namespace zdon_usio {

    struct Settings {
        std::string dll; // absolute, or relative to the game folder
        // a sensor at or above its threshold is a hit: P1 left kat, left don, right don, right kat, then P2
        std::array<int, 8> threshold{2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000};
        int holdoff_ms = 25; // after a hit, that side of that drum is ignored this long
        int poll_us = 1000;  // time between reads: under 1000 spins a core; 0 = read again at once
    };

    struct Sinks {
        // player 0/1, pad as EarlyHitPad (0 left don, 1 right don, 2 left kat, 3 right kat), strength ~0.5-1.15
        std::function<void(int player, int pad, float strength)> hit;
        // the test menu buttons as inject_menu_press takes them: -1 up (left), 1 down (right), 0 enter
        std::function<void(int action)> menu;
    };

    // Both sinks are called on the USIO thread.
    void start(const Settings& settings, Sinks sinks);
    void stop();
    bool running();

    // Game thread, once a frame: what arrived since the last call (coins and service credits, TEST presses)
    int take_coins();
    int take_service();
    int take_test_presses();

    // What the board reads now, for the USIO test screen (any thread). Channels per player: left kat, left don,
    // right don, right kat; P2 is 4-7.
    struct Snapshot {
        bool active = false; // the reader is running
        bool connected = false;
        std::string board;
        uint64_t firmware = 0;
        std::array<int, 8> threshold{};
        std::array<uint16_t, 8> value{};     // the latest reading
        std::array<uint16_t, 8> max_since{}; // the loudest since the previous snapshot() (none is missed)
        std::array<int, 8> hits{};           // hits counted per sensor that won its side
        std::array<uint16_t, 8> last_hit{};  // the value of each sensor's last hit
        bool has_switches = false;
        uint32_t switches = 0;
        int coin_total = -1; // the board's own running totals; -1 when it has no counter
        int service_total = -1;
        double reads_per_sec = 0;
    };
    Snapshot snapshot();

    // bnusio_GetSwIn bits (TaikoArcadeLoader's emulation)
    constexpr uint32_t SW_TEST = 1u << 7;
    constexpr uint32_t SW_ENTER = 1u << 9;
    constexpr uint32_t SW_DOWN = 1u << 12;
    constexpr uint32_t SW_UP = 1u << 13;
    constexpr uint32_t SW_SERVICE = 1u << 14;

} // namespace zdon_usio
