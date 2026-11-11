// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <vector>
#include "common/common_types.h"

namespace Cheats {
class CheatBase;
}

namespace Mercs3D {

/// Built-in game patches, run by the cheat engine next to the user's own cheats (they are not
/// shown in the cheat list and never written to the cheat file). Empty for other games.
std::vector<std::shared_ptr<Cheats::CheatBase>> BuiltinCheats(u64 title_id);

/// Whether a built-in patch should run now (follows the Mercs 3D settings).
bool BuiltinCheatEnabled(const Cheats::CheatBase& cheat);

/// If the game has no save yet, installs the "everything unlocked, no records" save.
void InstallDefaultSave(u64 title_id);

} // namespace Mercs3D
