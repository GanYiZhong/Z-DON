#pragma once

// Z-DON: the play screen seen in 3D ([general] zdon_lane3d, off by default). The frame is drawn as stacked layers,
// each into a target the size of the frame and each then drawn on its own plane at its own depth: the background
// at the back, then the lane, the notes, the judgments, the drum and score, the effects, the background's foreground
// and the screen's overlay in front. All planes turn together about the judge circle, so turning the view shows the
// layers apart (parallax); each casts a soft shadow on the ones behind and the far ones are dimmed. Each plane is
// scaled so that the flat view puts every pixel back where it was.
// Turned by dragging the mouse (left button: turn, wheel: distance about the judge circle, middle button: the next
// preset view, right button: back to flat; the view eases to where it is sent) and by skin scripts (tex.zdon_lane3d).
// Transitions and the pause menu stay flat on top. Judging is untouched. While the view is flat the screen draws as
// usual.
namespace zdon_lane3d {

    // back to front. LANE: the lane's art and the gauge. TARGET: the hit circle and what lights it (gogo flame, lane
    // flash, the lane cover and its mascot) and the notes' captions, on the notes' plane, so a note meets its circle
    // at any angle. NOTES: the notes and bar lines, drawn as coins (a body of slices behind each face). JUDGMENTS:
    // the judgment glow and text. HUD: the drum and its hit flashes, the soul arcs and flower, the ending, score,
    // combo, plate and labels, in their usual order. EFFECTS: the combo announce and the counters over the lane.
    enum class Layer {
        BACKGROUND,
        LANE,
        TARGET,
        NOTES,
        JUDGMENTS,
        HUD,
        EFFECTS,
        FOREGROUND,
        OVERLAY,
        COUNT,
        ALL
    };

    bool enabled();
    bool layered();        // between a begin_frame that returned true and its end_frame
    void reset();          // a new song: the starting view, mouse and script offsets cleared
    void release();        // the play screen ends: the targets' memory back
    void show_hint_once(); // the controls, the first time the view is used
    void update_mouse();   // once a frame on the play screen
    // Lua: the script's part of the view, reached over over_ms (0: at once); the mouse's part adds to it
    void set_script_view(float yaw_deg, float pitch_deg, float distance, double over_ms);
    void set_pivot(float virtual_x, float virtual_y); // the point the view turns about (the judge circle)

    // The frame as layers: begin_frame() returns false when the screen should draw as usual (flat or off); else
    // draw each layer between begin_layer and end_layer, then end_frame() draws them all, back to front.
    bool begin_frame();
    void begin_layer(Layer layer);
    void end_layer();
    void end_frame();

    // what Player::draw draws now: everything outside a layered frame, else the layer being drawn
    bool draws(Layer layer);

} // namespace zdon_lane3d
