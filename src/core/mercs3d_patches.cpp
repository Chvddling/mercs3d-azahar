// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <string>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/cheats/gateway_cheat.h"
#include "core/file_sys/archive_backend.h"
#include "core/mercs3d_default_save.h"
#include "core/mercs3d_layout.h"
#include "core/mercs3d_memory.h"
#include "core/mercs3d_patches.h"

namespace Mercs3D {

namespace {

enum class Group { Fps60, Gameplay, Widescreen };

struct Patch {
    const char* name;
    Group group;
    const char* code;
};

// Tested by the user as cheats (Gateway format). Code caves: 0x811D00-0x811DD3, 0x811E20-0x811E6F.
constexpr Patch PATCHES[] = {
    // 60 fps: frame cap (sMain +0x30 = 60, +0x2C = 30).
    {"60 fps", Group::Fps60,
     "D3000000 00000000\n"
     "B08BE7C0 00000000\n"
     "00000030 42700000\n"
     "0000002C 41F00000\n"
     "D2000000 00000000\n"},
    // Bullets travel the same distance per second at 60 fps (hit detection).
    {"60 fps bullet fix", Group::Fps60,
     "0058F578 ED901A00\n"
     "0058F57C EDD01A01\n"
     "0058F580 EDD00A02\n"
     "0058F584 EE210A01\n"
     "0058F588 EE010AA1\n"
     "0058F58C EE000AA0\n"
     "0058F590 EEB12AC0\n"
     "0058F594 EDD42A05\n"
     "0058F598 EE222A22\n"
     "0058F59C ED100A01\n"
     "0058F5CC 2D000A01\n"
     "0058F5D4 E320F000\n"
     "0058F5E0 E320F000\n"
     "0058F5E4 E320F000\n"
     "0058F5E8 EEC10A82\n"
     "0058F610 ED008A01\n"},
    // A grenade release only counts on the update its motion flag turns on.
    {"grenade double throw fix", Group::Fps60,
     "00811E60 E595C4C0\n"
     "00811E64 E1CBC00C\n"
     "00811E68 E31C0202\n"
     "00811E6C E12FFF1E\n"
     "0056E0E4 EB0A8F5D\n"},
    // Automatic weapons keep their 30 fps fire rate.
    {"auto fire rate fix", Group::Fps60,
     "00811E20 E2160602\n"
     "00811E24 0A00000B\n"
     "00811E28 E5950014\n"
     "00811E2C E35005FD\n"
     "00811E30 AA000007\n"
     "00811E34 E599011C\n"
     "00811E38 E3100602\n"
     "00811E3C 0A000005\n"
     "00811E40 ED990A13\n"
     "00811E44 EEBD0AC0\n"
     "00811E48 EE100A10\n"
     "00811E4C E3100001\n"
     "00811E50 E12FFF1E\n"
     "00811E54 E3B00001\n"
     "00811E58 E12FFF1E\n"
     "0056DFEC EB0A8F8B\n"},
    // D-pad Left / Right select the two grenade slots.
    {"grenades on D-pad", Group::Gameplay,
     "00811D80 E92D4013\n"
     "00811D84 E59F2044\n"
     "00811D88 E5922000\n"
     "00811D8C E5922094\n"
     "00811D90 E3120020\n"
     "00811D94 13A01004\n"
     "00811D98 1A000004\n"
     "00811D9C E3120080\n"
     "00811DA0 13A01005\n"
     "00811DA4 1A000001\n"
     "00811DA8 E8BD4013\n"
     "00811DAC EAFB9B61\n"
     "00811DB0 E5940DDC\n"
     "00811DB4 E3A02001\n"
     "00811DB8 EBF3B4FD\n"
     "00811DBC E3500000\n"
     "00811DC0 E8BD4013\n"
     "00811DC4 1AF5CA09\n"
     "00811DC8 E3A00000\n"
     "00811DCC E12FFF1E\n"
     "00811DD0 008BE7D8\n"
     "005845D4 EB0A35E9\n"},
    // True analog aiming: no body turn while aiming, no aim inertia.
    {"analog aim", Group::Gameplay,
     "006F8F2C EA046383\n"
     "00811D40 E92D401F\n"
     "00811D44 E1A00001\n"
     "00811D48 E1A01002\n"
     "00811D4C EBFB9DB7\n"
     "00811D50 E3500000\n"
     "00811D54 E8BD401F\n"
     "00811D58 0A000003\n"
     "00811D5C E3A03000\n"
     "00811D60 E5803000\n"
     "00811D64 E5803004\n"
     "00811D68 E12FFF1E\n"
     "00811D6C E92D4010\n"
     "00811D70 EAFB9C6E\n"
     "004D4578 EA000024\n"},
    // Wesker's ability on the Debug button (bound to R3 by the emulator) instead of D-pad Right.
    {"Wesker ability on R3", Group::Gameplay,
     "006F8A94 E2000001\n"
     "006F8A98 E1A02000\n"},
    // Field of view x1.4 (both third-person camera updates).
    {"field of view", Group::Gameplay,
     "005B2CAC EA097C13\n"
     "00811D00 ED9D0A20\n"
     "00811D04 EDDF2A01\n"
     "00811D08 EE200A22\n"
     "00811D0C EAF683E7\n"
     "00431F80 EA0F7F66\n"
     "00811D20 ED9D0A20\n"
     "00811D24 ED5F2A07\n"
     "00811D28 EE200A22\n"
     "00811D2C EAF08094\n"
     "00811D10 3FB33333\n"},
    // 16:9 render aspect for match cameras (uCameraManage), to go with the single screen layout.
    {"16:9 in matches", Group::Widescreen,
     "D3000000 00000000\n"
     "B08BE780 00000000\n"
     "B000002C 00000000\n"
     "50000000 00896E58\n"
     "00000038 3FE38E39\n"
     "D0000000 00000000\n"
     "D2000000 00000000\n"},
};

const Patch* FindPatch(const Cheats::CheatBase& cheat) {
    const std::string name = cheat.GetName();
    for (const Patch& patch : PATCHES) {
        if (name == patch.name) {
            return &patch;
        }
    }
    return nullptr;
}

} // namespace

std::vector<std::shared_ptr<Cheats::CheatBase>> BuiltinCheats(u64 title_id) {
    std::vector<std::shared_ptr<Cheats::CheatBase>> cheats;
    if (title_id != MERCS3D_TITLE_ID) {
        return cheats;
    }
    for (const Patch& patch : PATCHES) {
        auto cheat = std::make_shared<Cheats::GatewayCheat>(patch.name, patch.code, "");
        cheat->SetEnabled(true);
        cheats.push_back(std::move(cheat));
    }
    LOG_INFO(Core_Cheats, "Mercs3D: {} built-in patches loaded", cheats.size());
    return cheats;
}

bool BuiltinCheatEnabled(const Cheats::CheatBase& cheat) {
    const Patch* patch = FindPatch(cheat);
    if (patch == nullptr) {
        return false;
    }
    const LayoutSettings settings = GetLayoutSettings();
    switch (patch->group) {
    case Group::Fps60:
        return settings.fps60;
    case Group::Gameplay:
        return settings.game_patches;
    case Group::Widescreen:
        return settings.single_screen && settings.widescreen;
    }
    return false;
}

void InstallDefaultSave(u64 title_id) {
    if (title_id != MERCS3D_TITLE_ID) {
        return;
    }
    // Same layout as FileSys::ArchiveSource_SDSaveData (SD save data of an installed title).
    const std::string title_dir = fmt::format(
        "{}Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/"
        "title/{:08x}/{:08x}/",
        FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir), static_cast<u32>(title_id >> 32),
        static_cast<u32>(title_id & 0xFFFFFFFF));
    const std::string save_dir = title_dir + "data/00000001/";
    const std::string metadata_path = title_dir + "data/00000001.metadata";
    if (FileUtil::Exists(save_dir) || FileUtil::Exists(metadata_path)) {
        return; // The player already has a save: never touch it.
    }

    if (!FileUtil::CreateFullPath(save_dir)) {
        LOG_ERROR(Core, "Mercs3D: could not create {}", save_dir);
        return;
    }
    const std::string save_path = save_dir + "MER_SAVE.dat";
    FileUtil::IOFile save(save_path, "wb");
    if (!save.IsOpen() ||
        save.WriteBytes(DEFAULT_SAVE.data(), DEFAULT_SAVE.size()) != DEFAULT_SAVE.size()) {
        LOG_ERROR(Core, "Mercs3D: could not write {}", save_path);
        save.Close();
        FileUtil::DeleteDirRecursively(save_dir);
        return;
    }
    save.Close();

    // What a FormatSaveData call would have recorded: one 8400 byte file.
    FileSys::ArchiveFormatInfo format_info{};
    format_info.total_size = static_cast<u32>(DEFAULT_SAVE.size());
    format_info.number_directories = 0;
    format_info.number_files = 1;
    format_info.duplicate_data = 0;
    FileUtil::IOFile metadata(metadata_path, "wb");
    if (metadata.IsOpen()) {
        metadata.WriteBytes(&format_info, sizeof(format_info));
    }
    LOG_INFO(Core, "Mercs3D: installed the unlocked starter save in {}", save_dir);
}

} // namespace Mercs3D
