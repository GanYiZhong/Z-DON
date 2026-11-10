#pragma once
// Z-DON: in-game check for a new release on a self-hosted server (server/zdon_server.py publish) and the
// hand-off to zdon_update.ps1, which downloads only the changed files once the game has closed.
#include <string>

namespace zdon_update {

    struct Status {
        bool available = false;
        std::string version; // the server's version.txt
        int files = 0;       // files that differ from this install
    };

    // Starts the check on a background thread (once). No-op when config.toml has no update source or
    // [network] zdon_update_check is false.
    void start_check();
    Status status();

    // Starts zdon_update.ps1 (wait for this process, update, restart the game) in its own console.
    // True when it started; the caller then closes the game.
    bool launch_updater();

    void shutdown();

} // namespace zdon_update
