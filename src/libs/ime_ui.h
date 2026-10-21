#pragma once

#include <string>

#include "ray.h"

// Z-DON: the game draws the IME composition string and candidate list itself. SDL is told so with
// SDL_HINT_IME_IMPLEMENTED_UI, and the OS windows (hidden behind a fullscreen window) are not shown.
// The overlay is only up while a text field is edited (poll_text_edit / sync_text_input).

void ime_ui_set_hint();                      // before InitWindow (SDL reads it when text input first starts)
void ime_ui_install();                       // after InitWindow: watches the SDL text editing events
void ime_ui_text_input_changed(bool active); // sync_text_input started/stopped text input
// raylib queues only the first code point of an SDL text input event, but an IME commits whole
// phrases: the rest of each committed string, in order (poll_text_edit appends it).
std::string ime_ui_take_commit_tail();
void ime_ui_set_field(ray::Rectangle rect);    // virtual-screen rect of the field being edited, this frame
void ime_ui_draw(const ray::Camera2D& camera); // overlay, inside BeginMode2D(camera)

// Dev hook: YATAIDON_FAKE_IME="<f1>,<f2>" pushes a fake composition + candidate list at f1 and
// commits it at f2 (frames with YATAIDON_FIXED_STEP_MS, else ms), through SDL's own event queue.
void ime_ui_dev_tick(double since);
