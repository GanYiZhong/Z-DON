#pragma once

#include <chrono>

// Z-DON dev hook: YATAIDON_TEXT_STATS=1 logs, every 2 s, how often and how long the text code ran:
//   ctor     OutlinedText construction on the calling thread (font lookup + measuring; not the async build)
//   register FontManager::register_text (the song list loader thread rasterizes glyphs ahead of time)
//   atlas    GPU font atlas rebuilds / incremental updates (texture upload included)
//   upload   OutlinedText texture uploads
namespace text_stats {
    enum Kind {
        CTOR,
        REGISTER,
        ATLAS,
        UPLOAD,
        KIND_COUNT
    };

    bool enabled();
    void record(Kind kind, double ms);

    class Scope {
    public:
        explicit Scope(Kind kind) : kind(kind), on(enabled()) {
            if (on) {
                start = std::chrono::steady_clock::now();
            }
        }
        ~Scope() {
            if (on) {
                record(kind,
                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Kind kind;
        bool on;
        std::chrono::steady_clock::time_point start{};
    };
} // namespace text_stats
