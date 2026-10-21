// Z-DON: vblank-locked frame pacing (see vblank_pacing.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <vector>
// clang-format off
#include <windows.h>
#include <dwmapi.h> // must follow windows.h
// clang-format on

#include "../libs/time.h"
#include "platform_windows.h"
#include "vblank_pacing.h"

namespace vblank_pacing {
    namespace {

        using DwmTimingFn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);

        constexpr double kSlotTargetMs = 2.0; // spacing of the extra (input) frames within a refresh
        constexpr double kOnTimeMs = 0.5;     // a wake this close after the grid time takes the grid time
        // The timer wakes late (here 0.03-0.6 ms, p50 0.32: a 0.5 ms tick even for high-resolution timers). A
        // wake up to kOnTimeMs late still takes the grid time, so only the tail past that is spun away: the
        // spin is the 99th percentile of the lateness - kOnTimeMs + kSpinMarginMs, within the limits below.
        constexpr double kSpinMaxMs = 0.5; // also the spin until enough wakes were measured
        constexpr double kSpinMinMs = 0.05;
        constexpr double kSpinMarginMs = 0.1;

        struct State {
            bool active = false;
            int fixed_frames = 0; // ZDON_VBLANK_PACING=<n>; 0 = automatic
            DwmTimingFn dwm_timing = nullptr;
            double qpc_to_ms = 0.0;
            // vblank grid, in the get_current_ms timebase
            double vblank_ms = 0.0;
            double period_ms = 0.0;
            double last_query_ms = -1e18;
            // the shown frame starts `lead_ms` before each vblank
            double lead_ms = 2.5;
            double work_est = 1.75;   // a frame's work (start -> present end), 99.5th percentile
            std::vector<double> work; // samples for work_est and the lead
            double spin_ms = kSpinMaxMs;
            std::vector<double> late; // timer wake lateness samples for spin_ms
            bool precise_timer = true;
            double scheduled_ms = -1.0;
            double shown_vblank_ms = 0.0; // the vblank whose shown frame was scheduled last
            double wake_ms = 0.0;
            double last_frame_ms = 0.0;
            bool tear_free = true;     // ZDON_PACING_TEARFREE=0: present every frame, without vsync
            bool next_shown = true;    // the frame wait_next scheduled is the refresh's shown frame
            bool cur_shown = true;     // ... for the frame now running
            double work_end_ms = -1.0; // before_present() of the frame now ending
            FILE* log = nullptr;
        } S;

        bool query_grid(double now_ms) {
            DWM_TIMING_INFO ti{};
            ti.cbSize = sizeof(ti);
            LARGE_INTEGER q0;
            QueryPerformanceCounter(&q0);
            const double steady0 = get_current_ms();
            if (FAILED(S.dwm_timing(nullptr, &ti)) || ti.qpcRefreshPeriod == 0 || ti.qpcVBlank == 0) {
                return false;
            }
            const double period = ti.qpcRefreshPeriod * S.qpc_to_ms;
            if (period < 2.0 || period > 100.0) {
                return false;
            }
            // DWM reports the last vblank it composed on (stale while nothing changes on screen); it sits on
            // the grid, so extrapolating by the period is exact enough
            const double offset = steady0 - q0.QuadPart * S.qpc_to_ms;
            S.vblank_ms = ti.qpcVBlank * S.qpc_to_ms + offset;
            S.period_ms = period;
            S.last_query_ms = now_ms;
            if (S.log) {
                std::println(S.log, "v,{:.4f},{:.5f},{:.3f}", S.vblank_ms, S.period_ms, S.lead_ms);
            }
            return true;
        }

        // The next frame's start after `now_ms`. For each vblank D: the shown frame at D - lead, and before
        // it extra frames about kSlotTargetMs apart from the previous vblank on, each only if it is expected
        // to finish (work_est) before the shown frame's slot. A shown slot missed by a long frame still runs
        // at once if it can finish before D: then the result is -now_ms (start now, on the real time).
        double next_slot(double now_ms) {
            const double period = S.period_ms;
            const double lead = std::min(S.lead_ms, period);
            const double span = period - lead; // time before the shown frame's slot
            int extra = S.fixed_frames > 0 ? S.fixed_frames - 1 : (int)std::floor(span / kSlotTargetMs);
            extra = std::max(extra, 0);
            const double step = extra > 0 ? span / extra : 0.0;
            double k = std::floor((now_ms - S.vblank_ms) / period);
            for (int pass = 0; pass < 3; ++pass, k += 1.0) {
                const double start = S.vblank_ms + k * period; // the previous vblank
                const double vblank = start + period;
                if (std::abs(vblank - S.shown_vblank_ms) < period * 0.5) {
                    continue; // its shown frame already ran
                }
                const double shown = vblank - lead;
                for (int j = 0; j < extra; ++j) {
                    const double t = start + j * step;
                    if (t > now_ms && t + S.work_est <= shown) {
                        S.next_shown = false;
                        return t;
                    }
                }
                if (shown > now_ms || now_ms + S.work_est < vblank) {
                    S.shown_vblank_ms = vblank;
                    S.next_shown = true;
                    return shown > now_ms ? shown : -now_ms;
                }
            }
            S.next_shown = true;
            return -now_ms;
        }

        void update_lead() {
            if (S.work.size() < 600) {
                return;
            }
            std::vector<double> w = S.work;
            S.work.clear();
            const size_t q = w.size() * 995 / 1000;
            std::nth_element(w.begin(), w.begin() + q, w.end());
            S.work_est = w[q];
            const double want = std::clamp(w[q] + 0.75, 1.0, S.period_ms * 0.8);
            // grow at once, shrink slowly; small changes are ignored (each change shifts the shown frame's phase)
            if (want > S.lead_ms + 0.25) {
                S.lead_ms = want;
            } else if (want < S.lead_ms - 0.75) {
                S.lead_ms -= 0.25;
            }
        }

        void update_spin(double late_ms) {
            S.late.push_back(late_ms);
            if (S.late.size() < 600) {
                return;
            }
            const size_t q = S.late.size() * 99 / 100;
            std::nth_element(S.late.begin(), S.late.begin() + q, S.late.end());
            S.spin_ms = std::clamp(S.late[q] - kOnTimeMs + kSpinMarginMs, kSpinMinMs, kSpinMaxMs);
            S.late.clear();
        }

    } // namespace

    void init(bool vsync, double target_fps) {
        if (target_fps > 0.0) {
            return; // the fixed-rate limiter
        }
        if (fixed_step::on()) {
            return; // YATAIDON_FIXED_STEP_MS: frames run back to back
        }
        // On by default (owner's choice 2026-10-19). Trade-off: uncapped frames drop to about one per
        // refresh, and hits are judged at frame times. ZDON_VBLANK_PACING=0 turns it off; =<n> allows at
        // most n frames per refresh; unset or =auto picks the count automatically.
        // With vsync on (owner's choice 2026-10-22) it paces too, one frame per refresh as plain vsync:
        // NVIDIA's GL driver busy-waits for the flip at the next frame's first flush, so without pacing
        // the main thread spun a whole core (measured 95.6%, 5.46 ms of each 5.56 ms refresh). Pacing
        // sleeps until shortly before the vblank instead, which also starts each frame later (less lag).
        const char* env = std::getenv("ZDON_VBLANK_PACING");
        const bool off = env && *env == '0';
        if (env && *env && !off && std::string_view(env) != "auto") {
            S.fixed_frames = std::max(0, std::atoi(env));
        } else if (vsync) {
            S.fixed_frames = 1;
        }
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            S.dwm_timing = reinterpret_cast<DwmTimingFn>(
                reinterpret_cast<void*>(GetProcAddress(dwm, "DwmGetCompositionTimingInfo")));
        }
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        S.qpc_to_ms = 1000.0 / (double)f.QuadPart;
        if (!S.dwm_timing || !query_grid(get_current_ms())) {
            spdlog::info("vblank pacing: off (no DWM timing)");
            return;
        }
        // the log also works with pacing off, to compare against the free-running loop
        if (const char* lp = std::getenv("ZDON_PACING_LOG"); lp && *lp) {
            S.log = std::fopen(lp, "w");
        }
        if (off) {
            spdlog::info("vblank pacing: off (ZDON_VBLANK_PACING=0)");
            return;
        }
        S.active = true;
        const char* tf = std::getenv("ZDON_PACING_TEARFREE");
        S.tear_free = vsync || !(tf && *tf == '0'); // vsync on always presents with the driver's vsync
        spdlog::info("vblank pacing: on, refresh {:.3f} ms, frames per refresh {}, {}", S.period_ms,
                     S.fixed_frames > 0 ? std::to_string(S.fixed_frames) : std::string("auto"),
                     S.tear_free ? "tear-free (only the shown frame is presented, with vsync)"
                                 : "every frame presented without vsync (may tear)");
    }

    bool active() {
        return S.active;
    }

    double refresh_period_ms() {
        return S.active ? S.period_ms : 0.0;
    }

    bool present_this_frame() {
        return !S.active || !S.tear_free || S.cur_shown;
    }

    bool wants_vsync() {
        return S.active && S.tear_free;
    }

    void before_present() {
        if (S.active) {
            S.work_end_ms = get_current_ms();
        }
    }

    double frame_ms(double now_ms) {
        S.cur_shown = S.next_shown;
        S.wake_ms = now_ms;
        double t = now_ms;
        if (S.active && S.scheduled_ms > 0.0 && now_ms >= S.scheduled_ms && now_ms - S.scheduled_ms < kOnTimeMs &&
            S.scheduled_ms > S.last_frame_ms) {
            t = S.scheduled_ms;
        }
        S.last_frame_ms = t;
        return t;
    }

    void wait_next() {
        if (!S.active && !S.log) {
            return;
        }
        const double now = get_current_ms();
        if (S.log) {
            std::println(S.log, "f,{:.4f},{:.4f},{:.4f},{:.4f}", S.last_frame_ms, S.wake_ms, now, S.scheduled_ms);
        }
        if (now - S.last_query_ms > 500.0) {
            query_grid(now);
        }
        if (!S.active) {
            return;
        }
        // the work up to the swap: with vsync a shown frame's swap may wait for the refresh, which is not work
        // (it would push the lead up to the whole period)
        const double work_end = (S.work_end_ms > S.last_frame_ms && S.work_end_ms <= now) ? S.work_end_ms : now;
        S.work_end_ms = -1.0;
        S.work.push_back(work_end - S.last_frame_ms);
        update_lead();
        const double target = next_slot(now);
        if (target < 0.0) {
            S.scheduled_ms = -1.0; // run now, on its real start time
            return;
        }
        S.scheduled_ms = target;
        // without a high-resolution timer (before Windows 10 1803) the whole wait is spun, as before
        const double sleep_ms = target - S.spin_ms - now;
        if (sleep_ms > 0.0 && S.precise_timer) {
            S.precise_timer = win32_precise_sleep_us((long long)(sleep_ms * 1000.0));
            const double woke = get_current_ms();
            update_spin(woke - (now + sleep_ms));
            if (S.log) {
                std::println(S.log, "w,{:.4f},{:.4f},{:.3f}", now + sleep_ms, woke, S.spin_ms);
            }
        }
        while (get_current_ms() < target) {
            YieldProcessor();
        }
    }

} // namespace vblank_pacing
