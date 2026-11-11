// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

namespace Mercs3D {

/// If the game has no cheat file yet, writes the tested game patches (60 fps and its fixes,
/// grenades on the D-pad, analog aim, Wesker on R3, FOV, animation fix, 16:9) as enabled cheats.
void InstallDefaultCheats(u64 title_id);

/// If the game has no save yet, installs the "everything unlocked, no records" save.
void InstallDefaultSave(u64 title_id);

} // namespace Mercs3D
