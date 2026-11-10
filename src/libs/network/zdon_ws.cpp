#include "zdon_ws.h"

#include <chrono>
#include <curl/curl.h>
#include <spdlog/spdlog.h>

ZdonWs::~ZdonWs() {
    close();
}

void ZdonWs::open(std::string url, std::vector<std::string> headers) {
    close();
    stop_ = false;
    state_ = State::Connecting;
    thread_ = std::thread(&ZdonWs::run, this, std::move(url), std::move(headers));
}

void ZdonWs::send(std::string text) {
    std::scoped_lock lock(mutex_);
    outbox_.push_back(std::move(text));
}

std::optional<std::string> ZdonWs::poll() {
    std::scoped_lock lock(mutex_);
    if (inbox_.empty()) {
        return std::nullopt;
    }
    std::string m = std::move(inbox_.front());
    inbox_.pop_front();
    return m;
}

void ZdonWs::close() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ZdonWs::run(std::string url, std::vector<std::string> headers) {
    CURL* c = curl_easy_init();
    if (!c) {
        state_ = State::Failed;
        return;
    }
    curl_slist* list = nullptr;
    for (const std::string& h : headers) {
        list = curl_slist_append(list, h.c_str());
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L); // the WebSocket handshake, then curl_ws_send / curl_ws_recv
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_NODELAY, 1L);
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK || status != 101) {
        spdlog::warn("Online battle: no WebSocket ({}, HTTP {})", curl_easy_strerror(rc), status);
        state_ = State::Failed;
        curl_easy_cleanup(c);
        curl_slist_free_all(list);
        return;
    }
    state_ = State::Open;

    std::string message; // a text message being put together from its frames
    std::vector<char> buf(16384);
    bool alive = true;
    while (alive && !stop_) {
        std::deque<std::string> out;
        {
            std::scoped_lock lock(mutex_);
            out.swap(outbox_);
        }
        for (const std::string& m : out) {
            size_t sent = 0;
            CURLcode s = CURLE_AGAIN;
            for (int tries = 0; s == CURLE_AGAIN && tries < 2000 && !stop_; ++tries) {
                s = curl_ws_send(c, m.data(), m.size(), &sent, 0, CURLWS_TEXT);
                if (s == CURLE_AGAIN) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (s != CURLE_OK) {
                alive = false;
                break;
            }
        }
        bool got = false;
        while (alive) {
            size_t n = 0;
            const curl_ws_frame* meta = nullptr;
            const CURLcode r = curl_ws_recv(c, buf.data(), buf.size(), &n, &meta);
            if (r == CURLE_AGAIN) {
                break;
            }
            if (r != CURLE_OK || !meta || (meta->flags & CURLWS_CLOSE)) {
                alive = false;
                break;
            }
            got = true;
            if (meta->flags & (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CONT)) {
                message.append(buf.data(), n);
                if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
                    std::scoped_lock lock(mutex_);
                    inbox_.push_back(std::move(message));
                    message.clear();
                }
            }
        }
        if (!got && out.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (!stop_) {
        spdlog::info("Online battle: the WebSocket closed");
    } else {
        size_t sent = 0;
        curl_ws_send(c, "", 0, &sent, 0, CURLWS_CLOSE);
    }
    state_ = State::Closed;
    curl_easy_cleanup(c);
    curl_slist_free_all(list);
}
