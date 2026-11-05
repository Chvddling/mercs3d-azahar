// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>

namespace Core {
class System;
}

namespace Mercs3D {

/// One enemy marker on the bottom-screen minimap, in bottom screen pixels (320 x 240).
struct MinimapDot {
    float x;
    float y;
    bool other_floor; ///< The enemy is well above or below the player.
};

/// Display settings for the enemy markers, read from mercs3d.ini.
struct MinimapSettings {
    bool enabled = true;
    float dot_size = 3.0f; ///< Inner square size, in bottom screen pixels.
    float center_x = 240.0f;
    float center_y = 120.0f;
    float radius = 69.0f;        ///< Markers beyond this many pixels from the center are hidden.
    float floor_height = 250.0f; ///< Height difference (cm) that counts as another floor.
};

const MinimapSettings& GetMinimapSettings();

/**
 * Reads Resident Evil: The Mercenaries 3D's memory and returns where its enemies belong on the
 * minimap. Returns nothing when the game is not running or the minimap is not on screen.
 * Must be called from the emulation thread.
 */
std::vector<MinimapDot> GetMinimapDots(Core::System& system);

} // namespace Mercs3D
