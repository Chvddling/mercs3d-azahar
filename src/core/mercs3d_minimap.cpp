// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
#include <string>
#include "common/common_types.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "core/mercs3d_minimap.h"

namespace Mercs3D {

namespace {

// Resident Evil: The Mercenaries 3D (USA) (Rev 1)
constexpr u64 MERCS3D_TITLE_ID = 0x0004000000035900ULL;

// Globals (all in .bss/.data of the Rev 1 executable).
constexpr VAddr S_ID_COCKPIT = 0x008D586C; // sIDCockpit*
constexpr VAddr S_ENEMY = 0x008D594C;      // sEnemy*
constexpr VAddr S_PLAYER = 0x008D589C;     // sPlayer*
constexpr VAddr MAP_TABLE = 0x008DF158;    // per-stage world -> map scale table, 0x20 per stage
constexpr VAddr MAP_OFFSET = 0x008B5B08;   // vec2 added to the map translation

// Object layouts.
constexpr u32 COCKPIT_SUB = 0x28;        // sIDCockpit -> uGUI_CockpitSub
constexpr u32 SUB_MAP = 0x188;           // uGUI_CockpitSub -> uGUI_CockpitMap
constexpr u32 MAP_FLAGS = 0x158;         // bit 0: unit active
constexpr u32 MAP_STAGE = 0x17C;         // stage map index
constexpr u32 MAP_TEXTURE = 0x180;       // cGUIInstAnimVariable (the scrolling map)
constexpr u32 GUI_FLAGS = 0x58;          // bit 0: visible
constexpr u32 GUI_POS = 0x90;            // vec2 translation
constexpr u32 ENEMY_LIST = 0x20;         // sEnemy -> list sentinel
constexpr u32 PLAYER_LIST = 0x1C;        // sPlayer -> list sentinel
constexpr u32 NODE_NEXT = 0x4;           // list node -> next node
constexpr u32 NODE_OWNER = 0x10;         // list node -> character
constexpr u32 CHARA_POS = 0x40;          // vec3 world position (cm)
constexpr u32 PLAYER_IS_LOCAL = 0xCC0;   // u8, 1 for the player on this console
constexpr u32 CHICKEN_VTABLE = 0x884894; // uMerceChicken: not an enemy
constexpr int MAX_LIST_LENGTH = 128;

class Reader {
public:
    Reader(Memory::MemorySystem& memory_, const Kernel::Process& process_)
        : memory(memory_), process(process_) {}

    bool U32(VAddr addr, u32& out) const {
        if ((addr & 3) != 0 || !memory.IsValidVirtualAddress(process, addr) ||
            !memory.IsValidVirtualAddress(process, addr + 3)) {
            return false;
        }
        out = memory.Read32(process, addr);
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

    bool Ptr(VAddr addr, VAddr& out) const {
        u32 raw;
        if (!U32(addr, raw) || raw < 0x08000000 || raw >= 0x10000000) {
            return false;
        }
        out = raw;
        return true;
    }

private:
    Memory::MemorySystem& memory;
    const Kernel::Process& process;
};

/// The game's world -> map conversion (FUN_004acfb8): a separate scale for each sign of X and Z.
struct MapScale {
    float t[8];

    float Axis(float v, int axis) const {
        constexpr float K = 100.0f;
        const float a = t[axis];
        if (a != 0.0f && ((a > 0.0f) == (v > 0.0f))) {
            return -(v / (a * K)) * t[4 + axis];
        }
        const float b = t[2 + axis];
        if (b != 0.0f && ((b > 0.0f) == (v > 0.0f))) {
            return -(v / (b * K)) * t[6 + axis];
        }
        return 0.0f;
    }
};

MinimapSettings LoadSettings() {
    MinimapSettings s;
    const std::string path = FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "mercs3d.ini";
    std::string text;
    if (!FileUtil::Exists(path) || FileUtil::ReadFileToString(true, path, text) == 0) {
        return s;
    }
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        auto trim = [](std::string str) {
            const auto first = str.find_first_not_of(" \t\r");
            const auto last = str.find_last_not_of(" \t\r");
            return first == std::string::npos ? std::string{} : str.substr(first, last - first + 1);
        };
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        float number = 0.0f;
        try {
            number = std::stof(value);
        } catch (...) {
            continue;
        }
        if (key == "minimap_enemies") {
            s.enabled = number != 0.0f;
        } else if (key == "enemy_dot_size") {
            s.dot_size = std::clamp(number, 1.0f, 12.0f);
        } else if (key == "minimap_center_x") {
            s.center_x = number;
        } else if (key == "minimap_center_y") {
            s.center_y = number;
        } else if (key == "minimap_radius") {
            s.radius = std::max(number, 0.0f);
        } else if (key == "enemy_floor_height") {
            s.floor_height = std::max(number, 0.0f);
        }
    }
    LOG_INFO(Core, "Mercs3D: minimap enemies {}", s.enabled ? "on" : "off");
    return s;
}

} // namespace

const MinimapSettings& GetMinimapSettings() {
    static std::once_flag once;
    static MinimapSettings settings;
    std::call_once(once, [] { settings = LoadSettings(); });
    return settings;
}

std::vector<MinimapDot> GetMinimapDots(Core::System& system) {
    std::vector<MinimapDot> dots;
    const MinimapSettings& cfg = GetMinimapSettings();
    if (!cfg.enabled || !system.IsPoweredOn()) {
        return dots;
    }

    std::shared_ptr<Kernel::Process> game;
    for (const auto& p : system.Kernel().GetProcessList()) {
        if (p && p->codeset && p->codeset->program_id == MERCS3D_TITLE_ID) {
            game = p;
            break;
        }
    }
    if (!game) {
        return dots;
    }
    const Reader r(system.Memory(), *game);

    // Is the minimap up?
    VAddr cockpit, sub, map, texture;
    u32 map_flags, tex_flags, stage;
    if (!r.Ptr(S_ID_COCKPIT, cockpit) || !r.Ptr(cockpit + COCKPIT_SUB, sub) ||
        !r.Ptr(sub + SUB_MAP, map) || !r.U32(map + MAP_FLAGS, map_flags) || (map_flags & 1) == 0 ||
        !r.U32(map + MAP_STAGE, stage) || stage > 15 || !r.Ptr(map + MAP_TEXTURE, texture) ||
        !r.U32(texture + GUI_FLAGS, tex_flags) || (tex_flags & 1) == 0) {
        return dots;
    }

    // The map texture is translated by (offset + map position of the player), so the player's
    // map position can be read back from it without finding the player.
    MapScale scale;
    float off_x, off_y, tex_x, tex_y;
    for (int i = 0; i < 8; i++) {
        if (!r.F32(MAP_TABLE + stage * 0x20 + i * 4, scale.t[i])) {
            return dots;
        }
    }
    if (!r.F32(MAP_OFFSET, off_x) || !r.F32(MAP_OFFSET + 4, off_y) ||
        !r.F32(texture + GUI_POS, tex_x) || !r.F32(texture + GUI_POS + 4, tex_y)) {
        return dots;
    }
    const float player_map_x = tex_x - off_x;
    const float player_map_y = tex_y - off_y;

    // Player height, to tell enemies on other floors apart.
    bool have_player_y = false;
    float player_y = 0.0f;
    VAddr s_player;
    if (r.Ptr(S_PLAYER, s_player)) {
        const VAddr sentinel = s_player + PLAYER_LIST;
        VAddr node;
        bool ok = r.Ptr(sentinel + NODE_NEXT, node);
        for (int i = 0; ok && i < 8 && node != sentinel; i++) {
            VAddr chara;
            u32 local;
            if (r.Ptr(node + NODE_OWNER, chara) && r.U32(chara + PLAYER_IS_LOCAL, local) &&
                (local & 0xFF) == 1 && r.F32(chara + CHARA_POS + 4, player_y)) {
                have_player_y = true;
                break;
            }
            ok = r.Ptr(node + NODE_NEXT, node);
        }
    }

    VAddr s_enemy;
    if (!r.Ptr(S_ENEMY, s_enemy)) {
        return dots;
    }
    const VAddr sentinel = s_enemy + ENEMY_LIST;
    VAddr node;
    if (!r.Ptr(sentinel + NODE_NEXT, node)) {
        return dots;
    }
    for (int i = 0; i < MAX_LIST_LENGTH && node != sentinel; i++) {
        VAddr chara;
        u32 vtable;
        float x, y, z;
        if (r.Ptr(node + NODE_OWNER, chara) && r.U32(chara, vtable) && vtable != CHICKEN_VTABLE &&
            r.F32(chara + CHARA_POS, x) && r.F32(chara + CHARA_POS + 4, y) &&
            r.F32(chara + CHARA_POS + 8, z)) {
            const float dx = player_map_x - scale.Axis(x, 0);
            const float dy = player_map_y - scale.Axis(z, 1);
            if (std::fabs(dx) <= cfg.radius && std::fabs(dy) <= cfg.radius) {
                const bool other_floor =
                    have_player_y && std::fabs(y - player_y) > cfg.floor_height;
                dots.push_back({cfg.center_x + dx, cfg.center_y + dy, other_floor});
            }
        }
        if (!r.Ptr(node + NODE_NEXT, node)) {
            break;
        }
    }
    return dots;
}

} // namespace Mercs3D
