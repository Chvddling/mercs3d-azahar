// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/mercs3d_memory.h"
#include "core/mercs3d_spawn.h"

namespace Mercs3D {

namespace {

// Globals (Rev 1).
constexpr VAddr S_MISSION = 0x008D58D4; // sMission*
constexpr VAddr S_NETWORK = 0x008BE7C8; // network system*

constexpr u32 MISSION_ID = 0x18;        // sMission -> mMissionID (0x0501 = mission 6-2)
constexpr u32 MISSION_START_POS = 0x60; // sMission -> rStartPosition* (linear heap)
constexpr u32 START_POS_COUNT = 0x64;
constexpr u32 START_POS_POINTS = 0x70;  // PointData*[count]
constexpr u32 POINT_DATA_VTABLE = 0x00897D80;
constexpr u32 POINT_POS = 0x08;  // vec3
constexpr u32 POINT_YAW = 0x28;  // degrees
constexpr u32 POINT_USED = 0x30; // u8: taken by a player (set by the game when it spawns one)
constexpr u32 NETWORK_SESSION = 0x5C;
constexpr u32 SESSION_ACTIVE = 0xA10;

// The game picks a start point with FUN_005DF580: a few random tries for a point whose used
// byte is 0, then the first free one. Marking every other point as used makes it pick ours.

// 3DS pad bits (HID shared memory layout).
constexpr u32 PAD_A = 1u << 0;
constexpr u32 PAD_RIGHT = 1u << 4;
constexpr u32 PAD_LEFT = 1u << 5;
constexpr u32 PAD_Y = 1u << 11;
constexpr u32 PAD_CIRCLE_RIGHT = 1u << 28;
constexpr u32 PAD_CIRCLE_LEFT = 1u << 29;

std::mutex settings_mutex;
bool settings_loaded = false;
SpawnSettings settings;

std::mutex menu_mutex;
std::condition_variable menu_cv;
SpawnMenuFrontend* menu_frontend = nullptr;
bool answered = false;
SpawnChoice answer;
std::atomic<bool> cancelled{false};

VAddr handled_start_pos = 0; // emulation thread only

std::string IniPath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "mercs3d.ini";
}

std::string Trim(const std::string& str) {
    const auto first = str.find_first_not_of(" \t\r");
    const auto last = str.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string{} : str.substr(first, last - first + 1);
}

std::vector<std::string> ReadIniLines() {
    std::vector<std::string> lines;
    std::string text;
    const std::string path = IniPath();
    if (!FileUtil::Exists(path) || FileUtil::ReadFileToString(true, path, text) == 0) {
        return lines;
    }
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

/// Splits "key = value # comment" into key and value; false when the line holds no key.
bool ParseLine(const std::string& line, std::string& key, std::string& value) {
    std::string body = line;
    const auto comment = body.find_first_of("#;");
    if (comment != std::string::npos) {
        body.erase(comment);
    }
    const auto eq = body.find('=');
    if (eq == std::string::npos) {
        return false;
    }
    key = Trim(body.substr(0, eq));
    value = Trim(body.substr(eq + 1));
    return !key.empty();
}

constexpr const char* KEY_ASK = "spawn_menu";
constexpr const char* KEY_MISSION_PREFIX = "spawn_mission_";
constexpr const char* SECTION_COMMENT =
    "# Starting point menu. spawn_mission_<id> = remembered point (1, 2, ... or 0 = random)";

bool IsSpawnKey(const std::string& key) {
    return key == KEY_ASK || key.rfind(KEY_MISSION_PREFIX, 0) == 0;
}

void LoadLocked() {
    settings_loaded = true;
    settings = SpawnSettings{};
    for (const std::string& line : ReadIniLines()) {
        std::string key, value;
        if (!ParseLine(line, key, value)) {
            continue;
        }
        try {
            if (key == KEY_ASK) {
                settings.ask = std::stoi(value) != 0;
            } else if (key.rfind(KEY_MISSION_PREFIX, 0) == 0) {
                const u32 id = static_cast<u32>(
                    std::stoul(key.substr(std::string(KEY_MISSION_PREFIX).size()), nullptr, 16));
                settings.remembered[id] = std::max(std::stoi(value), 0) - 1;
            }
        } catch (...) {
        }
    }
}

bool InCoop(const Reader& r) {
    VAddr network, session;
    u32 active;
    return r.Ptr(S_NETWORK, network) && r.Ptr(network + NETWORK_SESSION, session) &&
           r.U32(session + SESSION_ACTIVE, active) && active != 0;
}

/// Blocks until the frontend answers (or the menu is cancelled), forwarding controller input.
SpawnChoice AskPlayer(const SpawnRequest& request, const std::function<u32()>& read_buttons) {
    std::unique_lock lock{menu_mutex};
    if (menu_frontend == nullptr || cancelled) {
        return {};
    }
    answered = false;
    SpawnMenuFrontend* frontend = menu_frontend;
    lock.unlock();
    frontend->OpenSpawnMenu(request);

    // Buttons already held when the menu opens do nothing until released.
    u32 previous = read_buttons();
    lock.lock();
    while (!answered && !cancelled) {
        menu_cv.wait_for(lock, std::chrono::milliseconds(16));
        if (answered || cancelled) {
            break;
        }
        lock.unlock();
        const u32 buttons = read_buttons();
        const u32 pressed = buttons & ~previous;
        previous = buttons;
        if (pressed & (PAD_LEFT | PAD_CIRCLE_LEFT)) {
            frontend->SpawnMenuInput(MenuInput::Previous);
        }
        if (pressed & (PAD_RIGHT | PAD_CIRCLE_RIGHT)) {
            frontend->SpawnMenuInput(MenuInput::Next);
        }
        if (pressed & PAD_Y) {
            frontend->SpawnMenuInput(MenuInput::ToggleRemember);
        }
        if (pressed & PAD_A) {
            frontend->SpawnMenuInput(MenuInput::Confirm);
        }
        lock.lock();
    }
    return answered ? answer : SpawnChoice{};
}

} // namespace

void SetSpawnMenuFrontend(SpawnMenuFrontend* frontend) {
    std::scoped_lock lock{menu_mutex};
    menu_frontend = frontend;
}

void AnswerSpawnMenu(const SpawnChoice& choice) {
    {
        std::scoped_lock lock{menu_mutex};
        answer = choice;
        answered = true;
    }
    menu_cv.notify_all();
}

void CancelSpawnMenu() {
    cancelled = true;
    menu_cv.notify_all();
}

void ResetSpawnMenu() {
    cancelled = false;
    handled_start_pos = 0;
}

void UpdateSpawnMenu(Core::System& system, const std::function<u32()>& read_buttons) {
    if (cancelled) {
        return;
    }
    const auto game = FindGameProcess(system);
    if (!game) {
        return;
    }
    const Reader r(*game);

    VAddr mission, start_pos, points;
    u32 count;
    if (!r.Ptr(S_MISSION, mission) || !r.AnyPtr(mission + MISSION_START_POS, start_pos) ||
        !r.U32(start_pos + START_POS_COUNT, count) || count < 2 || count > 8 ||
        !r.AnyPtr(start_pos + START_POS_POINTS, points)) {
        handled_start_pos = 0;
        return;
    }

    std::vector<VAddr> point_addrs;
    SpawnRequest request;
    bool any_used = false;
    for (u32 i = 0; i < count; i++) {
        VAddr p;
        u32 vtable;
        u8 used;
        SpawnPoint sp;
        if (!r.AnyPtr(points + i * 4, p) || !r.U32(p, vtable) || vtable != POINT_DATA_VTABLE ||
            !r.F32(p + POINT_POS, sp.x) || !r.F32(p + POINT_POS + 4, sp.y) ||
            !r.F32(p + POINT_POS + 8, sp.z) || !r.F32(p + POINT_YAW, sp.yaw) ||
            !r.U8(p + POINT_USED, used)) {
            return;
        }
        any_used |= used != 0;
        point_addrs.push_back(p);
        request.points.push_back(sp);
    }

    // A player has spawned: the next time every point is free again, ask again.
    if (any_used) {
        handled_start_pos = 0;
        return;
    }
    if (start_pos == handled_start_pos) {
        return;
    }
    handled_start_pos = start_pos;

    // Online co-op: the host picks both players' points; leave it to the game.
    if (InCoop(r)) {
        return;
    }
    if (!r.U32(mission + MISSION_ID, request.mission_id)) {
        return;
    }

    const SpawnSettings current = GetSpawnSettings();
    SpawnChoice choice;
    if (const auto it = current.remembered.find(request.mission_id);
        it != current.remembered.end()) {
        choice.point = it->second;
    } else if (current.ask) {
        choice = AskPlayer(request, read_buttons);
        if (choice.remember && !cancelled) {
            SpawnSettings updated = GetSpawnSettings();
            updated.remembered[request.mission_id] = choice.point;
            SetSpawnSettings(updated);
            SaveSpawnSettings();
        }
    } else {
        return;
    }

    if (choice.point < 0 || choice.point >= static_cast<int>(count)) {
        LOG_INFO(Core, "Mercs3D: {} starting point: random", MissionName(request.mission_id));
        return;
    }
    for (u32 i = 0; i < count; i++) {
        if (static_cast<int>(i) != choice.point) {
            r.WriteU8(point_addrs[i] + POINT_USED, 1);
        }
    }
    LOG_INFO(Core, "Mercs3D: {} starting point {}", MissionName(request.mission_id),
             choice.point + 1);
}

SpawnSettings GetSpawnSettings() {
    std::scoped_lock lock{settings_mutex};
    if (!settings_loaded) {
        LoadLocked();
    }
    return settings;
}

void SetSpawnSettings(const SpawnSettings& new_settings) {
    std::scoped_lock lock{settings_mutex};
    settings_loaded = true;
    settings = new_settings;
}

void SaveSpawnSettings() {
    std::scoped_lock lock{settings_mutex};
    if (!settings_loaded) {
        LoadLocked();
    }
    std::vector<std::string> lines;
    for (std::string& line : ReadIniLines()) {
        std::string key, value;
        if (line == SECTION_COMMENT || (ParseLine(line, key, value) && IsSpawnKey(key))) {
            continue;
        }
        lines.push_back(std::move(line));
    }
    while (!lines.empty() && Trim(lines.back()).empty()) {
        lines.pop_back();
    }
    lines.push_back("");
    lines.push_back(SECTION_COMMENT);
    lines.push_back(fmt::format("{} = {}", KEY_ASK, settings.ask ? 1 : 0));
    for (const auto& [id, point] : settings.remembered) {
        lines.push_back(fmt::format("{}{:04x} = {}", KEY_MISSION_PREFIX, id, point + 1));
    }
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += '\n';
    }
    if (FileUtil::WriteStringToFile(true, IniPath(), out) != out.size()) {
        LOG_ERROR(Core, "Mercs3D: could not write {}", IniPath());
    }
}

std::string MissionName(u32 mission_id) {
    return fmt::format("Mission {}-{}", ((mission_id >> 8) & 0xFF) + 1, (mission_id & 0xFF) + 1);
}

} // namespace Mercs3D
