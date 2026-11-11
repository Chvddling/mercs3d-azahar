// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Mercs3D {

/// A starting point of the mission being loaded (game units: cm, yaw in degrees).
struct SpawnPoint {
    float x, y, z, yaw;
};

struct SpawnRequest {
    u32 mission_id;                 ///< 0x0501 = mission 6-2
    std::vector<SpawnPoint> points; ///< Two or more.
};

/// Controller input while the starting point menu is open.
enum class MenuInput { Previous, Next, Confirm, ToggleRemember };

/// What the player picked. point = -1 means random (the game's own choice).
struct SpawnChoice {
    int point = -1;
    bool remember = false;
};

/// Implemented by the frontend. All calls come from the emulation thread; the frontend
/// answers with AnswerSpawnMenu (from any thread).
class SpawnMenuFrontend {
public:
    virtual ~SpawnMenuFrontend() = default;
    virtual void OpenSpawnMenu(const SpawnRequest& request) = 0;
    virtual void SpawnMenuInput(MenuInput input) = 0;
};

void SetSpawnMenuFrontend(SpawnMenuFrontend* frontend);
void AnswerSpawnMenu(const SpawnChoice& choice);

/// Stops a menu that is waiting for an answer (and any later one) so the emulation thread
/// can finish. Call before stopping emulation.
void CancelSpawnMenu();
/// Allows the menu again after CancelSpawnMenu. Call when a game starts.
void ResetSpawnMenu();

/// Called by the HID pad update (emulation thread). Watches the mission being loaded and,
/// right before the game picks a starting point, asks the player (blocking emulation).
/// read_buttons returns the current 3DS pad bits (HID layout).
void UpdateSpawnMenu(Core::System& system, const std::function<u32()>& read_buttons);

struct SpawnSettings {
    bool ask = true;               ///< Show the menu when a mission with 2+ points loads.
    std::map<u32, int> remembered; ///< mission id -> point index (-1 = random)
};

SpawnSettings GetSpawnSettings();
void SetSpawnSettings(const SpawnSettings& settings);
/// Writes the spawn settings to mercs3d.ini, keeping its other lines.
void SaveSpawnSettings();

/// "Mission 6-2"
std::string MissionName(u32 mission_id);

} // namespace Mercs3D
