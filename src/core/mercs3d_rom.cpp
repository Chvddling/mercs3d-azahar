// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <mutex>
#include <cryptopp/sha.h>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/mercs3d_memory.h"
#include "core/mercs3d_rom.h"

namespace Mercs3D {

namespace {

// SHA-256 of the decompressed ExeFS .code of
// "Resident Evil - The Mercenaries 3D (USA) (En,Fr) (Rev 1)" (8,216,576 bytes), the release
// every address in this fork comes from.
constexpr const char* EXPECTED_SHA256 =
    "374f6878d5c032c9a40be8cd9d138aab4c47a5a70f5202c0e7afea8457fcea2b";

constexpr const char* PROCESS_NAME = "biomerce"; // exheader codeset name, all regions

std::atomic<RomCheck> result{RomCheck::OtherGame};
std::mutex hash_mutex;
std::string last_hash;

} // namespace

void CheckGameCode(u64 program_id, const std::string& process_name, const std::vector<u8>& code) {
    if (program_id != MERCS3D_TITLE_ID && process_name != PROCESS_NAME) {
        result = RomCheck::OtherGame;
        return;
    }
    u8 digest[CryptoPP::SHA256::DIGESTSIZE];
    CryptoPP::SHA256().CalculateDigest(digest, code.data(), code.size());
    std::string hex;
    for (const u8 byte : digest) {
        hex += fmt::format("{:02x}", byte);
    }
    {
        std::scoped_lock lock{hash_mutex};
        last_hash = hex;
    }
    const bool ok = program_id == MERCS3D_TITLE_ID && hex == EXPECTED_SHA256;
    result = ok ? RomCheck::Compatible : RomCheck::Incompatible;
    if (ok) {
        LOG_INFO(Loader, "Mercs3D: supported release (USA Rev 1), code SHA-256 {}", hex);
    } else {
        LOG_WARNING(Loader,
                    "Mercs3D: unsupported release (title {:016x}, code SHA-256 {}, expected {}). "
                    "Game patches, starter save and game memory features are off.",
                    program_id, hex, EXPECTED_SHA256);
    }
}

void ResetRomCheck() {
    result = RomCheck::OtherGame;
    std::scoped_lock lock{hash_mutex};
    last_hash.clear();
}

RomCheck GetRomCheck() {
    return result.load();
}

std::string GetRomHash() {
    std::scoped_lock lock{hash_mutex};
    return last_hash;
}

bool IsSupportedRomLoaded() {
    return result.load(std::memory_order_relaxed) == RomCheck::Compatible;
}

std::string ExpectedRomHash() {
    return EXPECTED_SHA256;
}

} // namespace Mercs3D
