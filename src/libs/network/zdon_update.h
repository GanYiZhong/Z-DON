#pragma once
// Z-DON: in-game check for a new release on a self-hosted server (server/zdon_server.py publish) and the
// hand-off to zdon_update.ps1, which downloads only the changed files once the game has closed.
#include <optional>
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

    // Starts the updater (wait for this process, update, restart the game): zdon_update.ps1 in its own console on
    // Windows, zdon_update.sh in the background on Linux / macOS. True when it started; the caller then closes the
    // game.
    bool launch_updater();

    void shutdown();

    // The Z-DON server: config.toml [network] zdon_server_url, else the compiled-in NETWORK_URL; no trailing
    // slash, empty when there is neither.
    std::string server_base();

    // A blocking GET: the body when the answer was 200, else nullopt (status receives the HTTP status, 0 when
    // there was no answer). Always nullopt on builds without an HTTP client.
    std::optional<std::string> http_get(const std::string& url, int timeout_ms, int* status = nullptr);

} // namespace zdon_update
