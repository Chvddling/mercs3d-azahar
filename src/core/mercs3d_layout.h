// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace Core {
class System;
}

namespace Mercs3D {

/// What the game is showing, as far as the screen layout is concerned.
enum class GameScreen {
    Other,   ///< Menus, pause menu, loading: both screens as usual.
    Match,   ///< In a match: top screen only, with parts of the bottom screen on top of it.
    Results, ///< Results screen: top screen only, nothing on top.
};

/// A part of the bottom screen shown on top of the top screen during a match.
struct Overlay {
    bool enabled = true;
    float x = 0.0f;     ///< Left edge, as a fraction of the game area's width.
    float y = 0.0f;     ///< Top edge, as a fraction of the game area's height.
    float size = 0.3f;  ///< Height, as a fraction of the game area's height.
    float alpha = 1.0f; ///< 0 = invisible, 1 = solid.
};

/// Source rectangle on the bottom screen, in bottom screen pixels (320 x 240).
struct BottomRegion {
    float x, y, w, h;
};

// Inset a few pixels so the overlay does not pick up the panel border around the map.
constexpr BottomRegion MINIMAP_REGION{166.0f, 44.0f, 150.0f, 152.0f};
constexpr BottomRegion ITEMS_REGION{2.0f, 10.0f, 138.0f, 219.0f};

struct LayoutSettings {
    bool single_screen = true; ///< Use the single screen layout in matches and on results.
    bool widescreen = true;    ///< Game area is 16:9 (letterboxed); otherwise it fills the window.
    Overlay minimap{true, 0.779f, 0.012f, 0.344f, 1.0f};
    Overlay items{true, 0.0f, 0.677f, 0.323f, 1.0f};
    /// Controller type B (RE5 style): ZL aims, ZR fires while aiming, and while aiming the
    /// right stick (C-Stick) aims instead of the left stick.
    bool controller_type_b = false;
};

/// Current settings (loaded from mercs3d.ini on first use). Thread safe.
LayoutSettings GetLayoutSettings();

/// Changes the settings in use right away, without saving them.
void SetLayoutSettings(const LayoutSettings& settings);

/// Writes the current settings to mercs3d.ini, keeping its other lines.
void SaveLayoutSettings();

/// Reads the game's memory to tell what it is showing. Call from the emulation thread.
GameScreen GetGameScreen(Core::System& system);

/// Set by the renderer: whether the last frame used the single screen layout.
void SetSingleScreenShown(bool shown);
bool IsSingleScreenShown();

} // namespace Mercs3D
