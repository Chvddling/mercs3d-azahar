// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include "common/common_types.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"

namespace Mercs3D {

/// Resident Evil: The Mercenaries 3D (USA) (Rev 1)
constexpr u64 MERCS3D_TITLE_ID = 0x0004000000035900ULL;

/// Only the supported release (checked by hash in mercs3d_rom.cpp) gets the memory features.
bool IsSupportedRomLoaded();

inline std::shared_ptr<Kernel::Process> FindGameProcess(Core::System& system) {
    if (!system.IsPoweredOn() || !IsSupportedRomLoaded()) {
        return nullptr;
    }
    for (const auto& p : system.Kernel().GetProcessList()) {
        if (p && p->codeset && p->codeset->program_id == MERCS3D_TITLE_ID) {
            return p;
        }
    }
    return nullptr;
}

/// Reads the game's memory from outside the CPU (e.g. while presenting a frame).
class Reader {
public:
    explicit Reader(const Kernel::Process& process_)
        : page_table(*process_.vm_manager.page_table) {}

    // Reads only pages backed by plain memory. Going through MemorySystem::Read would flush the
    // GPU cache for GPU-owned pages, which crashes when done in the middle of presenting a frame.
    bool U32(VAddr addr, u32& out) const {
        if ((addr & 3) != 0) {
            return false;
        }
        const u8* page = page_table.pointers[addr >> Memory::CITRA_PAGE_BITS];
        if (page == nullptr) {
            return false;
        }
        std::memcpy(&out, page + (addr & Memory::CITRA_PAGE_MASK), sizeof(out));
        return true;
    }

    bool F32(VAddr addr, float& out) const {
        u32 raw;
        if (!U32(addr, raw)) {
            return false;
        }
        out = std::bit_cast<float>(raw);
        return std::isfinite(out);
    }

    bool U8(VAddr addr, u8& out) const {
        const u8* page = page_table.pointers[addr >> Memory::CITRA_PAGE_BITS];
        if (page == nullptr) {
            return false;
        }
        out = page[addr & Memory::CITRA_PAGE_MASK];
        return true;
    }

    /// Writes one byte of game data (plain memory only). Call from the emulation thread.
    bool WriteU8(VAddr addr, u8 value) const {
        u8* page = page_table.pointers[addr >> Memory::CITRA_PAGE_BITS];
        if (page == nullptr) {
            return false;
        }
        page[addr & Memory::CITRA_PAGE_MASK] = value;
        return true;
    }

    /// Reads a pointer into the application heap.
    bool Ptr(VAddr addr, VAddr& out) const {
        u32 raw;
        if (!U32(addr, raw) || raw < 0x08000000 || raw >= 0x10000000) {
            return false;
        }
        out = raw;
        return true;
    }

    /// Reads a pointer into the application heap or the linear heap.
    bool AnyPtr(VAddr addr, VAddr& out) const {
        u32 raw;
        if (!U32(addr, raw)) {
            return false;
        }
        const bool heap = raw >= 0x08000000 && raw < 0x10000000;
        const bool linear = raw >= 0x14000000 && raw < 0x1C000000;
        if (!heap && !linear) {
            return false;
        }
        out = raw;
        return true;
    }

private:
    Memory::PageTable& page_table;
};

} // namespace Mercs3D
