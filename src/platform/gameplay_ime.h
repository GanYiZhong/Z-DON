#pragma once

// Z-DON: Windows text services (TSF) off during gameplay.
//
// IMM32 activates TSF for the main thread when the window is created. Even with the window's input context
// detached (SDL does that whenever text input is stopped, so also during gameplay), TSF keeps running for the
// thread: WM_TIMER callbacks in MSCTF.dll (about 5 ms each) and messages to its helper windows run inside
// SDL_PumpEvents and stall the frame. ImmDisableIME before the window exists removes all of it, but for
// good (no IME typing in the song select search). Here, on gameplay screens, that activation is taken back
// (ITfThreadMgr::Deactivate) and given back with the same flags on leaving, so IME typing still works
// everywhere else. Each switch costs one frame 30-65 ms (at the screen change, before the song starts / when
// the result screen opens). ZDON_GAMEPLAY_IME=1 leaves TSF on during gameplay.

#include "../libs/screen.h"

namespace gameplay_ime {
    inline bool is_gameplay(Screens s) {
        switch (s) {
        case Screens::GAME:
        case Screens::GAME_2P:
        case Screens::GAME_DAN:
        case Screens::GAME_PRACTICE:
        case Screens::AI_GAME:
        case Screens::RETRY_GAME:
            return true;
        default:
            return false;
        }
    }
#ifdef _WIN32
    // once per frame, after the window exists: detaches on entering gameplay, re-attaches on leaving
    void update(bool gameplay);
#else
    inline void update(bool) {}
#endif
} // namespace gameplay_ime
