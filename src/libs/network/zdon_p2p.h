#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Z-DON live battle: a direct UDP connection between the two games, next to the server's WebSocket.
//   start(key): opens a UDP socket for IPv4 and one for IPv6 and gathers this machine's addresses: the local ones,
//               and the public IPv4 address and port a STUN server sees (the NAT's mapping of the same socket).
//   candidates(): those addresses ("ip:port", "[ipv6]:port") once gathered, for the opponent (through the server).
//   add_remote(): the opponent's addresses: both sides then send probes to every one of them at once, which also
//               opens each side's NAT for the other (hole punching). The first address that answers is used.
//   send() / poll(): datagrams (small text messages) once connected(); lost ones are not sent again here (the
//               caller's messages carry everything not yet acknowledged).
// Every packet carries the round's key from the server: packets without it are ignored. If no address answers,
// connected() stays false and the caller keeps to the WebSocket.
class ZdonP2P {
public:
    ZdonP2P() = default;
    ZdonP2P(const ZdonP2P&) = delete;
    ZdonP2P& operator=(const ZdonP2P&) = delete;
    ~ZdonP2P();

    void start(const std::string& key);
    std::optional<std::vector<std::string>> candidates();
    void add_remote(const std::vector<std::string>& addresses);
    bool connected() const { return connected_.load(); }
    std::string peer() const; // the address in use ("" before connected)
    void send(std::string text);
    std::optional<std::string> poll();
    void stop();

private:
    void run();

    std::string key_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::optional<std::vector<std::string>> local_; // gathered addresses
    std::vector<std::string> remote_;               // the opponent's
    bool remote_new_ = false;
    std::string peer_;
    std::deque<std::string> outbox_, inbox_;
};
