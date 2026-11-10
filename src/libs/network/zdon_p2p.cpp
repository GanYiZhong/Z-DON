#include "zdon_p2p.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <random>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using zsock = SOCKET;
static constexpr zsock kNoSock = INVALID_SOCKET;
static void close_sock(zsock s) {
    closesocket(s);
}
static void set_nonblocking(zsock s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using zsock = int;
static constexpr zsock kNoSock = -1;
static void close_sock(zsock s) {
    close(s);
}
static void set_nonblocking(zsock s) {
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}
#endif

namespace {
    using Clock = std::chrono::steady_clock;
    constexpr const char* kMagic = "ZD1"; // every packet: magic, kind, key, payload
    constexpr int kProbeMs = 100;         // probes to every address of the opponent this often
    constexpr int kKeepAliveMs = 1000;    // once connected (keeps the NAT's mapping open)
    constexpr const char* kStun[] = {"stun.l.google.com", "stun.cloudflare.com"};
    constexpr const char* kStunPort[] = {"19302", "3478"};

    // what kind of address "ip:port" / "[ipv6]:port" is, for the log (the addresses themselves are not logged)
    const char* addr_kind(const std::string& text) {
        if (text.starts_with("[")) {
            return "IPv6";
        }
        unsigned a = 0, b = 0;
        if (std::sscanf(text.c_str(), "%u.%u.", &a, &b) != 2) {
            return "other";
        }
        if (a == 10 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31)) {
            return "LAN";
        }
        if (a == 100 && b >= 64 && b <= 127) {
            return "carrier NAT";
        }
        return "public";
    }

    std::string kinds(const std::vector<std::string>& list) {
        std::vector<std::pair<std::string, int>> n;
        for (const std::string& t : list) {
            const std::string k = addr_kind(t);
            auto it = std::ranges::find_if(n, [&k](const auto& e) { return e.first == k; });
            if (it == n.end()) {
                n.emplace_back(k, 1);
            } else {
                ++it->second;
            }
        }
        std::string out;
        for (const auto& [k, c] : n) {
            out += std::format("{}{} {}", out.empty() ? "" : ", ", c, k);
        }
        return out.empty() ? "none" : out;
    }

    std::string addr_text(const sockaddr_storage& a) {
        char ip[INET6_ADDRSTRLEN] = {};
        if (a.ss_family == AF_INET) {
            const auto& v4 = reinterpret_cast<const sockaddr_in&>(a);
            inet_ntop(AF_INET, &v4.sin_addr, ip, sizeof ip);
            return std::string(ip) + ":" + std::to_string(ntohs(v4.sin_port));
        }
        const auto& v6 = reinterpret_cast<const sockaddr_in6&>(a);
        inet_ntop(AF_INET6, &v6.sin6_addr, ip, sizeof ip);
        return "[" + std::string(ip) + "]:" + std::to_string(ntohs(v6.sin6_port));
    }

    bool parse_addr(const std::string& text, sockaddr_storage& out, socklen_t& len) {
        std::memset(&out, 0, sizeof out);
        const size_t colon = text.rfind(':');
        if (colon == std::string::npos) {
            return false;
        }
        std::string host = text.substr(0, colon);
        const int port = std::atoi(text.c_str() + colon + 1);
        if (port <= 0 || port > 65535) {
            return false;
        }
        if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
            auto& v6 = reinterpret_cast<sockaddr_in6&>(out);
            v6.sin6_family = AF_INET6;
            v6.sin6_port = htons(static_cast<uint16_t>(port));
            len = sizeof(sockaddr_in6);
            return inet_pton(AF_INET6, host.c_str(), &v6.sin6_addr) == 1;
        }
        auto& v4 = reinterpret_cast<sockaddr_in&>(out);
        v4.sin_family = AF_INET;
        v4.sin_port = htons(static_cast<uint16_t>(port));
        len = sizeof(sockaddr_in);
        return inet_pton(AF_INET, host.c_str(), &v4.sin_addr) == 1;
    }

    zsock open_udp(int family, uint16_t& port) {
        zsock s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kNoSock) {
            return s;
        }
        sockaddr_storage a{};
        socklen_t len = 0;
        if (family == AF_INET6) {
            int only = 1;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&only), sizeof only);
            auto& v6 = reinterpret_cast<sockaddr_in6&>(a);
            v6.sin6_family = AF_INET6;
            v6.sin6_addr = in6addr_any;
            len = sizeof(sockaddr_in6);
        } else {
            auto& v4 = reinterpret_cast<sockaddr_in&>(a);
            v4.sin_family = AF_INET;
            v4.sin_addr.s_addr = htonl(INADDR_ANY);
            len = sizeof(sockaddr_in);
        }
        if (bind(s, reinterpret_cast<sockaddr*>(&a), len) != 0) {
            close_sock(s);
            return kNoSock;
        }
        len = sizeof a;
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(family == AF_INET6 ? reinterpret_cast<sockaddr_in6&>(a).sin6_port
                                        : reinterpret_cast<sockaddr_in&>(a).sin_port);
        set_nonblocking(s);
        return s;
    }

    // this machine's addresses (no loopback, link-local or APIPA)
    std::vector<sockaddr_storage> local_addresses() {
        std::vector<sockaddr_storage> out;
        char name[256] = {};
        if (gethostname(name, sizeof name) != 0) {
            return out;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(name, nullptr, &hints, &res) != 0) {
            return out;
        }
        for (addrinfo* p = res; p; p = p->ai_next) {
            sockaddr_storage a{};
            std::memcpy(&a, p->ai_addr, p->ai_addrlen);
            if (a.ss_family == AF_INET) {
                const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in&>(a).sin_addr.s_addr);
                if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) {
                    continue;
                }
            } else if (a.ss_family == AF_INET6) {
                const auto& b = reinterpret_cast<sockaddr_in6&>(a).sin6_addr;
                const auto* bytes = reinterpret_cast<const uint8_t*>(&b);
                if ((bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80) || IN6_IS_ADDR_LOOPBACK(&b)) {
                    continue;
                }
            } else {
                continue;
            }
            out.push_back(a);
        }
        freeaddrinfo(res);
        return out;
    }

    // a STUN binding request (RFC 5389) from `s`: the public address the server saw, or nothing
    std::optional<sockaddr_storage> stun_public_v4(zsock s) {
        std::mt19937 rng(std::random_device{}());
        for (size_t server = 0; server < std::size(kStun); ++server) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(kStun[server], kStunPort[server], &hints, &res) != 0 || !res) {
                continue;
            }
            std::array<uint8_t, 20> req{0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
            for (size_t i = 8; i < 20; ++i) {
                req[i] = static_cast<uint8_t>(rng());
            }
            for (int attempt = 0; attempt < 3; ++attempt) {
                sendto(s, reinterpret_cast<const char*>(req.data()), static_cast<int>(req.size()), 0, res->ai_addr,
                       static_cast<socklen_t>(res->ai_addrlen));
                const auto until = Clock::now() + std::chrono::milliseconds(400);
                while (Clock::now() < until) {
                    std::array<uint8_t, 512> buf{};
                    sockaddr_storage from{};
                    socklen_t flen = sizeof from;
                    const int n =
                        static_cast<int>(recvfrom(s, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()),
                                                  0, reinterpret_cast<sockaddr*>(&from), &flen));
                    if (n < 20) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                    if (buf[0] != 0x01 || buf[1] != 0x01 || std::memcmp(buf.data() + 8, req.data() + 8, 12) != 0) {
                        continue;
                    }
                    // attributes: XOR-MAPPED-ADDRESS (0x0020), or MAPPED-ADDRESS (0x0001)
                    for (int at = 20; at + 4 <= n;) {
                        const int type = (buf[at] << 8) | buf[at + 1];
                        const int len = (buf[at + 2] << 8) | buf[at + 3];
                        const int v = at + 4;
                        if ((type == 0x0020 || type == 0x0001) && len >= 8 && v + 8 <= n && buf[v + 1] == 0x01) {
                            const bool x = type == 0x0020;
                            uint16_t port = static_cast<uint16_t>((buf[v + 2] << 8) | buf[v + 3]);
                            uint32_t ip = (static_cast<uint32_t>(buf[v + 4]) << 24) | (buf[v + 5] << 16) |
                                          (buf[v + 6] << 8) | buf[v + 7];
                            if (x) {
                                port ^= 0x2112;
                                ip ^= 0x2112A442u;
                            }
                            sockaddr_storage a{};
                            auto& v4 = reinterpret_cast<sockaddr_in&>(a);
                            v4.sin_family = AF_INET;
                            v4.sin_port = htons(port);
                            v4.sin_addr.s_addr = htonl(ip);
                            freeaddrinfo(res);
                            return a;
                        }
                        at = v + ((len + 3) & ~3);
                    }
                }
            }
            freeaddrinfo(res);
        }
        return std::nullopt;
    }
} // namespace

ZdonP2P::~ZdonP2P() {
    stop();
}

void ZdonP2P::start(const std::string& key) {
    stop();
    key_ = key;
    stop_ = false;
    connected_ = false;
    thread_ = std::thread(&ZdonP2P::run, this);
}

void ZdonP2P::stop() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::optional<std::vector<std::string>> ZdonP2P::candidates() {
    std::scoped_lock lock(mutex_);
    return local_;
}

void ZdonP2P::add_remote(const std::vector<std::string>& addresses) {
    std::scoped_lock lock(mutex_);
    remote_ = addresses;
    remote_new_ = true;
}

std::string ZdonP2P::peer() const {
    std::scoped_lock lock(mutex_);
    return peer_;
}

void ZdonP2P::send(std::string text) {
    std::scoped_lock lock(mutex_);
    if (outbox_.size() < 64) {
        outbox_.push_back(std::move(text));
    }
}

std::optional<std::string> ZdonP2P::poll() {
    std::scoped_lock lock(mutex_);
    if (inbox_.empty()) {
        return std::nullopt;
    }
    std::string m = std::move(inbox_.front());
    inbox_.pop_front();
    return m;
}

void ZdonP2P::run() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    uint16_t port4 = 0, port6 = 0;
    const zsock s4 = open_udp(AF_INET, port4);
    const zsock s6 = open_udp(AF_INET6, port6);
    std::vector<std::string> mine;
    for (const sockaddr_storage& a : local_addresses()) {
        sockaddr_storage b = a;
        if (b.ss_family == AF_INET && s4 != kNoSock) {
            reinterpret_cast<sockaddr_in&>(b).sin_port = htons(port4);
        } else if (b.ss_family == AF_INET6 && s6 != kNoSock) {
            reinterpret_cast<sockaddr_in6&>(b).sin6_port = htons(port6);
        } else {
            continue;
        }
        mine.push_back(addr_text(b));
    }
    if (s4 != kNoSock) {
        if (auto pub = stun_public_v4(s4)) {
            const std::string text = addr_text(*pub);
            if (std::ranges::find(mine, text) == mine.end()) {
                mine.insert(mine.begin(), text);
            }
        }
    }
    spdlog::info("Online battle: direct connection addresses: {} ({})", mine.size(), kinds(mine));
    {
        std::scoped_lock lock(mutex_);
        local_ = mine;
    }

    const std::string head = std::string(kMagic);
    auto packet = [&](char kind, const std::string& payload) { return head + kind + key_ + payload; };
    std::vector<std::pair<sockaddr_storage, socklen_t>> targets;
    std::optional<std::pair<sockaddr_storage, socklen_t>> peer;
    auto send_to = [&](const std::pair<sockaddr_storage, socklen_t>& t, const std::string& data) {
        const zsock s = t.first.ss_family == AF_INET6 ? s6 : s4;
        if (s != kNoSock) {
            sendto(s, data.data(), static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&t.first),
                   t.second);
        }
    };
    auto last_probe = Clock::now() - std::chrono::seconds(10);
    auto probe_since = Clock::now();
    bool told_none = false;
    std::vector<char> buf(2048);
    while (!stop_) {
        {
            std::scoped_lock lock(mutex_);
            if (remote_new_) {
                remote_new_ = false;
                spdlog::info("Online battle: the opponent's addresses: {} ({})", remote_.size(), kinds(remote_));
                probe_since = Clock::now();
                targets.clear();
                for (const std::string& r : remote_) {
                    sockaddr_storage a{};
                    socklen_t len = 0;
                    if (parse_addr(r, a, len)) {
                        targets.emplace_back(a, len);
                    }
                }
            }
        }
        const auto now = Clock::now();
        const int every = peer ? kKeepAliveMs : kProbeMs;
        if (!targets.empty() && now - last_probe >= std::chrono::milliseconds(every)) {
            last_probe = now;
            if (!peer && !told_none && now - probe_since > std::chrono::seconds(10)) {
                told_none = true;
                spdlog::warn(
                    "Online battle: no direct connection: none of the opponent's {} address(es) answered in 10 s "
                    "(a firewall, or a NAT that doesn't let it through); the server carries the hits",
                    targets.size());
            }
            if (peer) {
                send_to(*peer, packet('P', ""));
            } else {
                for (const auto& t : targets) {
                    send_to(t, packet('P', ""));
                }
            }
        }
        if (peer) {
            std::deque<std::string> out;
            {
                std::scoped_lock lock(mutex_);
                out.swap(outbox_);
            }
            for (const std::string& m : out) {
                send_to(*peer, packet('D', m));
            }
        }
        // read both sockets (a few ms at most)
        fd_set rd;
        FD_ZERO(&rd);
        zsock top = 0;
        for (zsock s : {s4, s6}) {
            if (s != kNoSock) {
                FD_SET(s, &rd);
                top = std::max(top, s);
            }
        }
        timeval tv{0, 2000};
        if (select(static_cast<int>(top + 1), &rd, nullptr, nullptr, &tv) <= 0) {
            continue;
        }
        for (zsock s : {s4, s6}) {
            if (s == kNoSock || !FD_ISSET(s, &rd)) {
                continue;
            }
            while (true) {
                sockaddr_storage from{};
                socklen_t flen = sizeof from;
                const int n = static_cast<int>(recvfrom(s, buf.data(), static_cast<int>(buf.size()), 0,
                                                        reinterpret_cast<sockaddr*>(&from), &flen));
                if (n <= 0) {
                    break;
                }
                const std::string_view p(buf.data(), static_cast<size_t>(n));
                const size_t hl = head.size() + 1 + key_.size();
                if (p.size() < hl || p.substr(0, head.size()) != head ||
                    p.substr(head.size() + 1, key_.size()) != key_) {
                    continue;
                }
                const char kind = p[head.size()];
                if (!peer) { // the first address that answers is the one
                    peer = std::make_pair(from, flen);
                    {
                        std::scoped_lock lock(mutex_);
                        peer_ = addr_text(from);
                    }
                    connected_ = true;
                    spdlog::info("Online battle: direct connection to the opponent ({} address)",
                                 addr_kind(addr_text(from)));
                }
                if (kind == 'P') {
                    send_to(std::make_pair(from, flen), packet('R', ""));
                } else if (kind == 'D') {
                    std::scoped_lock lock(mutex_);
                    if (inbox_.size() < 256) {
                        inbox_.emplace_back(p.substr(hl));
                    }
                }
            }
        }
    }
    for (zsock s : {s4, s6}) {
        if (s != kNoSock) {
            close_sock(s);
        }
    }
#ifdef _WIN32
    WSACleanup();
#endif
}
