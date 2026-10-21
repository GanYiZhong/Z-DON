// Z-DON: Windows text services (TSF) off during gameplay (see gameplay_ime.h).
#include <chrono>
#include <cstdlib>
#include <spdlog/spdlog.h>
// clang-format off
#include <windows.h>
#include <msctf.h>
// clang-format on

#include "gameplay_ime.h"

namespace gameplay_ime {
    namespace {
        // defined here: the MinGW import libraries do not have them all
        const CLSID kClsidThreadMgr = {0x529a9e6b, 0x6587, 0x4f23, {0xab, 0x9e, 0x9c, 0x7d, 0x68, 0x3e, 0x3c, 0x50}};
        const IID kIidThreadMgrEx = {0x3e90ade3, 0x7594, 0x4cb0, {0xbb, 0x58, 0x69, 0x62, 0x8f, 0x5f, 0x45, 0x8c}};
        constexpr DWORD kActivateFlagsMask = 0x7f; // the TF_TMF_* bits that ActivateEx takes back (TF_TMAE_*)

        struct State {
            bool init = false;
            bool detached = false;    // on a gameplay screen
            bool deactivated = false; // we deactivated TSF and activate it again on leaving
            DWORD flags = 0;          // its activation flags before
            ITfThreadMgrEx* tim = nullptr;
        };
        State g;

        double ms_since(std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        }

        void init_once() {
            g.init = true;
            if (const char* v = std::getenv("ZDON_GAMEPLAY_IME"); v && *v == '1') {
                spdlog::info("gameplay IME: ZDON_GAMEPLAY_IME=1, text services stay on during gameplay");
                return;
            }
            // SDL initialized COM on this thread already; this only adds a reference (kept for the process)
            (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            // the thread's own thread manager (one per thread: this returns the one IMM32 activated)
            if (FAILED(CoCreateInstance(kClsidThreadMgr, nullptr, CLSCTX_INPROC_SERVER, kIidThreadMgrEx,
                                        reinterpret_cast<void**>(&g.tim)))) {
                g.tim = nullptr;
                spdlog::info("gameplay IME: no TSF thread manager, nothing to turn off");
            }
        }
    } // namespace

    void update(bool gameplay) {
        if (!g.init) {
            init_once();
        }
        if (!g.tim || gameplay == g.detached) {
            return;
        }
        g.detached = gameplay;
        const auto t0 = std::chrono::steady_clock::now();
        if (gameplay) {
            // IMM32 activated TSF for this thread when the window was created. TSF counts activations:
            // taking that one away stops TSF for the thread, as ImmDisableIME does before the window exists.
            DWORD flags = 0;
            if (FAILED(g.tim->GetActiveFlags(&flags)) || !(flags & TF_TMF_ACTIVATED)) {
                return;
            }
            g.deactivated = SUCCEEDED(g.tim->Deactivate());
            g.flags = flags & kActivateFlagsMask;
            spdlog::info("gameplay IME: text services off ({:.2f} ms)", ms_since(t0));
        } else if (g.deactivated) {
            // the same activation back (IMM32 deactivates it when the window goes)
            TfClientId id = 0;
            const HRESULT hr = g.tim->ActivateEx(&id, g.flags);
            g.deactivated = false;
            spdlog::info("gameplay IME: text services on ({:.2f} ms, hr {:x})", ms_since(t0),
                         static_cast<unsigned>(hr));
        }
    }
} // namespace gameplay_ime
