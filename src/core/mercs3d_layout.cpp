// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/mercs3d_layout.h"
#include "core/mercs3d_memory.h"

namespace Mercs3D {

namespace {

// Globals (Rev 1).
constexpr VAddr S_ID_COCKPIT = 0x008D586C; // sIDCockpit*
constexpr VAddr S_GAME_AREA = 0x008BE77C;  // sGameArea*

constexpr u32 GAME_AREA_SCENE = 0x3828;         // sGameArea -> current scene (aXxx)
constexpr u32 RESULT_SCENE_VTABLE = 0x0087F738; // aMerceResult
constexpr u32 COCKPIT_SUB = 0x28;               // sIDCockpit -> uGUI_CockpitSub
constexpr u32 COCKPIT_PAUSE = 0x3C;             // sIDCockpit -> uPauseMainMerce
constexpr u32 SUB_MAP = 0x188;                  // uGUI_CockpitSub -> uGUI_CockpitMap
constexpr u32 UNIT_FLAGS = 0x158;               // GUI unit flags, bit 0: active
constexpr u32 MAP_TEXTURE = 0x180;              // uGUI_CockpitMap -> map texture
constexpr u32 GUI_FLAGS = 0x58;                 // GUI object flags, bit 0: visible

std::mutex mutex;
std::atomic<bool> single_screen_shown{false};
bool loaded = false;
LayoutSettings current;

std::string IniPath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "mercs3d.ini";
}

std::string Trim(const std::string& str) {
    const auto first = str.find_first_not_of(" \t\r");
    const auto last = str.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string{} : str.substr(first, last - first + 1);
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        pos = end + 1;
    }
    return lines;
}

/// Key name of an ini line, or empty when the line holds no key.
std::string KeyOf(const std::string& line) {
    std::string body = line;
    const auto comment = body.find_first_of("#;");
    if (comment != std::string::npos) {
        body.erase(comment);
    }
    const auto eq = body.find('=');
    return eq == std::string::npos ? std::string{} : Trim(body.substr(0, eq));
}

struct Entry {
    const char* key;
    float* value;
    float min;
    float max;
};

// Float view of the settings, so loading and saving share one table.
struct FloatSettings {
    float single_screen, widescreen;
    float map_on, map_x, map_y, map_size, map_alpha;
    float items_on, items_x, items_y, items_size, items_alpha;
    float type_b, fps60, game_patches;

    explicit FloatSettings(const LayoutSettings& s)
        : single_screen(s.single_screen), widescreen(s.widescreen), map_on(s.minimap.enabled),
          map_x(s.minimap.x), map_y(s.minimap.y), map_size(s.minimap.size),
          map_alpha(s.minimap.alpha), items_on(s.items.enabled), items_x(s.items.x),
          items_y(s.items.y), items_size(s.items.size), items_alpha(s.items.alpha),
          type_b(s.controller_type_b), fps60(s.fps60), game_patches(s.game_patches) {}

    LayoutSettings ToSettings() const {
        LayoutSettings s;
        s.single_screen = single_screen != 0.0f;
        s.widescreen = widescreen != 0.0f;
        s.minimap = {map_on != 0.0f, map_x, map_y, map_size, map_alpha};
        s.items = {items_on != 0.0f, items_x, items_y, items_size, items_alpha};
        s.controller_type_b = type_b != 0.0f;
        s.fps60 = fps60 != 0.0f;
        s.game_patches = game_patches != 0.0f;
        return s;
    }

    std::array<Entry, 15> Entries() {
        return {{
            {"single_screen", &single_screen, 0.0f, 1.0f},
            {"widescreen", &widescreen, 0.0f, 1.0f},
            {"minimap_overlay", &map_on, 0.0f, 1.0f},
            {"minimap_x", &map_x, -1.0f, 2.0f},
            {"minimap_y", &map_y, -1.0f, 2.0f},
            {"minimap_size", &map_size, 0.02f, 1.0f},
            {"minimap_alpha", &map_alpha, 0.0f, 1.0f},
            {"items_overlay", &items_on, 0.0f, 1.0f},
            {"items_x", &items_x, -1.0f, 2.0f},
            {"items_y", &items_y, -1.0f, 2.0f},
            {"items_size", &items_size, 0.02f, 1.0f},
            {"items_alpha", &items_alpha, 0.0f, 1.0f},
            {"controller_type_b", &type_b, 0.0f, 1.0f},
            {"fps60", &fps60, 0.0f, 1.0f},
            {"game_patches", &game_patches, 0.0f, 1.0f},
        }};
    }
};

void LoadLocked() {
    loaded = true;
    FloatSettings f{LayoutSettings{}};
    std::string text;
    const std::string path = IniPath();
    if (!FileUtil::Exists(path) || FileUtil::ReadFileToString(true, path, text) == 0) {
        current = f.ToSettings();
        return;
    }
    auto entries = f.Entries();
    for (const std::string& line : SplitLines(text)) {
        const std::string key = KeyOf(line);
        if (key.empty()) {
            continue;
        }
        std::string body = line.substr(line.find('=') + 1);
        const auto comment = body.find_first_of("#;");
        if (comment != std::string::npos) {
            body.erase(comment);
        }
        for (auto& e : entries) {
            if (key == e.key) {
                try {
                    *e.value = std::clamp(std::stof(Trim(body)), e.min, e.max);
                } catch (...) {
                }
            }
        }
    }
    current = f.ToSettings();
}

} // namespace

LayoutSettings GetLayoutSettings() {
    std::scoped_lock lock{mutex};
    if (!loaded) {
        LoadLocked();
    }
    return current;
}

void SetLayoutSettings(const LayoutSettings& settings) {
    std::scoped_lock lock{mutex};
    loaded = true;
    current = settings;
}

void SaveLayoutSettings() {
    std::scoped_lock lock{mutex};
    if (!loaded) {
        LoadLocked();
    }
    FloatSettings f{current};
    auto entries = f.Entries();
    std::vector<bool> written(entries.size(), false);

    const std::string path = IniPath();
    std::string text;
    if (FileUtil::Exists(path)) {
        FileUtil::ReadFileToString(true, path, text);
    }
    std::vector<std::string> lines = SplitLines(text);
    for (std::string& line : lines) {
        const std::string key = KeyOf(line);
        for (std::size_t i = 0; i < entries.size(); i++) {
            if (key == entries[i].key) {
                line = fmt::format("{} = {}", entries[i].key, *entries[i].value);
                written[i] = true;
            }
        }
    }
    if (std::find(written.begin(), written.end(), false) != written.end()) {
        lines.push_back("");
        lines.push_back("# Mercs 3D settings (also editable in Emulation > Configure > Mercs 3D)");
        for (std::size_t i = 0; i < entries.size(); i++) {
            if (!written[i]) {
                lines.push_back(fmt::format("{} = {}", entries[i].key, *entries[i].value));
            }
        }
    }
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += '\n';
    }
    if (FileUtil::WriteStringToFile(true, path, out) != out.size()) {
        LOG_ERROR(Core, "Mercs3D: could not write {}", path);
    }
}

GameScreen GetGameScreen(Core::System& system) {
    const auto game = FindGameProcess(system);
    if (!game) {
        return GameScreen::Other;
    }
    const Reader r(*game);

    VAddr area, scene;
    u32 scene_vtable;
    if (r.Ptr(S_GAME_AREA, area) && r.Ptr(area + GAME_AREA_SCENE, scene) &&
        r.U32(scene, scene_vtable) && scene_vtable == RESULT_SCENE_VTABLE) {
        return GameScreen::Results;
    }

    // In a match the cockpit GUI exists and its minimap is up.
    VAddr cockpit, sub, map, texture;
    u32 map_flags, tex_flags;
    if (!r.Ptr(S_ID_COCKPIT, cockpit) || !r.Ptr(cockpit + COCKPIT_SUB, sub) ||
        !r.Ptr(sub + SUB_MAP, map) || !r.U32(map + UNIT_FLAGS, map_flags) || (map_flags & 1) == 0 ||
        !r.Ptr(map + MAP_TEXTURE, texture) || !r.U32(texture + GUI_FLAGS, tex_flags) ||
        (tex_flags & 1) == 0) {
        return GameScreen::Other;
    }
    // The pause menu uses both screens.
    VAddr pause;
    u32 pause_flags;
    if (r.Ptr(cockpit + COCKPIT_PAUSE, pause) && r.U32(pause + UNIT_FLAGS, pause_flags) &&
        (pause_flags & 1) != 0) {
        return GameScreen::Other;
    }
    return GameScreen::Match;
}

void SetSingleScreenShown(bool shown) {
    single_screen_shown.store(shown, std::memory_order_relaxed);
}

bool IsSingleScreenShown() {
    return single_screen_shown.load(std::memory_order_relaxed);
}

} // namespace Mercs3D
