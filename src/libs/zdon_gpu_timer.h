#pragma once

// Z-DON dev hook: ZDON_GPU_TIMER=1 timestamps the GPU at a few points of each frame (GL_TIMESTAMP queries, read
// back a few frames later) and logs every 2 s the average GPU milliseconds between them: frame start -> screen
// draw, the screen's own draw, overlays -> present, and from one frame's start to the next. Each mark flushes the
// draw batch first (only while the hook is on). Off: one getenv at startup, nothing per frame.
namespace zdon_gpu_timer {
    enum Mark {
        FRAME_START = 0,
        DRAW_START,
        DRAW_END,
        PRESENT,
        kMarks
    };
    bool on();
    void mark(Mark m);
    void frame_end(); // after the present: collects finished frames, logs
} // namespace zdon_gpu_timer
