// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <string>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/file_sys/archive_backend.h"
#include "core/mercs3d_default_cheats.h"
#include "core/mercs3d_default_save.h"
#include "core/mercs3d_memory.h"
#include "core/mercs3d_patches.h"

namespace Mercs3D {

void InstallDefaultCheats(u64 title_id) {
    if (title_id != MERCS3D_TITLE_ID || !IsSupportedRomLoaded()) {
        return;
    }
    const std::string dir = FileUtil::GetUserPath(FileUtil::UserPath::CheatsDir);
    const std::string path = fmt::format("{}{:016X}.txt", dir, title_id);
    if (FileUtil::Exists(path)) {
        return; // Never replace the player's own cheat list.
    }
    FileUtil::CreateFullPath(dir);
    const std::string text = DEFAULT_CHEATS;
    if (FileUtil::WriteStringToFile(true, path, text) != text.size()) {
        LOG_ERROR(Core, "Mercs3D: could not write {}", path);
        return;
    }
    LOG_INFO(Core, "Mercs3D: installed the game patch cheats in {}", path);
}

void InstallDefaultSave(u64 title_id) {
    if (title_id != MERCS3D_TITLE_ID || !IsSupportedRomLoaded()) {
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
