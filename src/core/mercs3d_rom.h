// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>
#include "common/common_types.h"

namespace Mercs3D {

enum class RomCheck {
    OtherGame,    ///< Not Resident Evil: The Mercenaries 3D.
    Compatible,   ///< The supported release: (USA) (En,Fr) (Rev 1).
    Incompatible, ///< Mercenaries 3D, but another region / revision / modified code.
};

/// Called by the NCCH loader with the game's decompressed .code. Hashes it (SHA-256) and
/// remembers whether it is the release the patches were made for.
void CheckGameCode(u64 program_id, const std::string& process_name, const std::vector<u8>& code);

/// Forget the last result (called before loading any game).
void ResetRomCheck();

RomCheck GetRomCheck();
/// SHA-256 of the last checked code, hex.
std::string GetRomHash();
/// The hash the patches expect, hex.
std::string ExpectedRomHash();

} // namespace Mercs3D
