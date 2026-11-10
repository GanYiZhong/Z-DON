#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Z-DON: a WebSocket client (curl's curl_ws_*) on its own thread, for the live online battle: text messages
// both ways, pushed the moment they come (no polling). open() connects in the background; send() queues a
// message; poll() takes one that came. A connection that fails or drops ends in Failed / Closed for good.
class ZdonWs {
public:
    enum class State {
        Idle,
        Connecting,
        Open,
        Failed,
        Closed
    };

    ZdonWs() = default;
    ZdonWs(const ZdonWs&) = delete;
    ZdonWs& operator=(const ZdonWs&) = delete;
    ~ZdonWs();

    // url: ws:// or wss://; headers: "Name: value" lines sent with the handshake
    void open(std::string url, std::vector<std::string> headers);
    State state() const { return state_.load(); }
    void send(std::string text);
    std::optional<std::string> poll();
    void close();

private:
    void run(std::string url, std::vector<std::string> headers);

    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<std::string> outbox_, inbox_;
};
