// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <string>
#include <boost/serialization/array.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/unique_ptr.hpp>
#include "common/archives.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/kernel/shared_page.h"
#include "core/hle/service/hid/hid.h"
#include "core/hle/service/hid/hid_spvr.h"
#include "core/hle/service/hid/hid_user.h"
#include "core/hle/service/ir/ir_rst.h"
#include "core/hle/service/ir/ir_user.h"
#include "core/hle/service/service.h"
#include "core/memory.h"
#include "core/mercs3d_layout.h"
#include "core/mercs3d_spawn.h"
#include "core/movie.h"

SERVICE_CONSTRUCT_IMPL(Service::HID::Module)
SERIALIZE_EXPORT_IMPL(Service::HID::Module)

namespace Service::HID {

template <class Archive>
void Module::serialize(Archive& ar, const unsigned int file_version) {
    DEBUG_SERIALIZATION_POINT;
    ar & shared_mem;
    ar & event_pad_or_touch_1;
    ar & event_pad_or_touch_2;
    ar & event_accelerometer;
    ar & event_gyroscope;
    ar & event_debug_pad;
    ar & next_pad_index;
    ar & next_touch_index;
    ar & next_accelerometer_index;
    ar & next_gyroscope_index;
    ar & enable_accelerometer_count;
    ar & enable_gyroscope_count;
    if (Archive::is_loading::value) {
        LoadInputDevices();
    }
    ar & state.hex;
    ar & circle_pad_old_x;
    ar & circle_pad_old_y;
    // Update events are set in the constructor
    // Devices are set from the implementation (and are stateless afaik)
}
SERIALIZE_IMPL(Module)

ArticBaseController::ArticBaseController(
    const std::shared_ptr<Network::ArticBase::Client>& client) {

    udp_stream =
        client->NewUDPStream("ArticController", sizeof(ArticBaseController::ControllerData),
                             std::chrono::milliseconds(2));
    if (udp_stream.get()) {
        udp_stream->Start();
    }
}

ArticBaseController::ControllerData ArticBaseController::GetControllerData() {

    if (udp_stream.get() && udp_stream->IsReady()) {
        auto data = udp_stream->GetLastPacket();
        if (data.size() == sizeof(ControllerData)) {
            u32 id = *reinterpret_cast<u32*>(data.data());
            if ((id - last_packet_id) < (std::numeric_limits<u32>::max() / 2)) {
                last_packet_id = id;
                memcpy(&last_controller_data, data.data(), data.size());
            }
        }
    }
    return last_controller_data;
}

constexpr float accelerometer_coef = 512.0f; // measured from hw test result
constexpr float gyroscope_coef = 14.375f; // got from hwtest GetGyroscopeLowRawToDpsCoefficient call

DirectionState GetStickDirectionState(s16 circle_pad_x, s16 circle_pad_y) {
    // 30 degree and 60 degree are angular thresholds for directions
    constexpr float TAN30 = 0.577350269f;
    constexpr float TAN60 = 1 / TAN30;
    // a circle pad radius greater than 40 will trigger circle pad direction
    constexpr int CIRCLE_PAD_THRESHOLD_SQUARE = 40 * 40;
    DirectionState state{false, false, false, false};

    if (circle_pad_x * circle_pad_x + circle_pad_y * circle_pad_y > CIRCLE_PAD_THRESHOLD_SQUARE) {
        float t = std::abs(static_cast<float>(circle_pad_y) / circle_pad_x);

        if (circle_pad_x != 0 && t < TAN60) {
            if (circle_pad_x > 0)
                state.right = true;
            else
                state.left = true;
        }

        if (circle_pad_x == 0 || t > TAN30) {
            if (circle_pad_y > 0)
                state.up = true;
            else
                state.down = true;
        }
    }

    return state;
}

void Module::LoadInputDevices() {
    LOG_DEBUG(Frontend, "Loading input devices");
    std::transform(Settings::values.current_input_profile.buttons.begin() +
                       Settings::NativeButton::BUTTON_HID_BEGIN,
                   Settings::values.current_input_profile.buttons.begin() +
                       Settings::NativeButton::BUTTON_HID_END,
                   buttons.begin(), Input::CreateDevice<Input::ButtonDevice>);
    circle_pad = Input::CreateDevice<Input::AnalogDevice>(
        Settings::values.current_input_profile.analogs[Settings::NativeAnalog::CirclePad]);
    mercs3d_c_stick = Input::CreateDevice<Input::AnalogDevice>(
        Settings::values.current_input_profile.analogs[Settings::NativeAnalog::CStick]);
    mercs3d_zl = Input::CreateDevice<Input::ButtonDevice>(
        Settings::values.current_input_profile.buttons[Settings::NativeButton::ZL]);
    mercs3d_zr = Input::CreateDevice<Input::ButtonDevice>(
        Settings::values.current_input_profile.buttons[Settings::NativeButton::ZR]);
    LoadMercs3DConfig();
    motion_device = Input::CreateDevice<Input::MotionDevice>(
        Settings::values.current_input_profile.motion_device);
    touch_device = Input::CreateDevice<Input::TouchDevice>(
        Settings::values.current_input_profile.touch_device);
    if (Settings::values.current_input_profile.use_touchpad &&
        Settings::values.current_input_profile.controller_touch_device != "") {
        controller_touch_device = Input::CreateDevice<Input::TouchDevice>(
            Settings::values.current_input_profile.controller_touch_device);
    } else {
        controller_touch_device.reset();
    }
    if (Settings::values.current_input_profile.use_touch_from_button) {
        touch_btn_device = Input::CreateDevice<Input::TouchDevice>("engine:touch_from_button");
    } else {
        touch_btn_device.reset();
    }
}

void Module::LoadMercs3DConfig() {
    const std::string path = FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "mercs3d.ini";
    mercs3d_config = Mercs3DConfig{};

    std::string text;
    if (!FileUtil::Exists(path) || FileUtil::ReadFileToString(true, path, text) == 0) {
        const std::string defaults =
            "# Mercs3D settings for Resident Evil: The Mercenaries 3D.\n"
            "# Edit and restart the game to apply. Delete this file to restore defaults.\n"
            "\n"
            "# Right stick (mapped to C-Stick) drags a virtual finger on the bottom screen,\n"
            "# which drives the look-around camera like dragging on the minimap.\n"
            "right_stick_camera = 1\n"
            "# Stick deadzone, 0.0 to 0.9\n"
            "deadzone = 0.15\n"
            "# Where the virtual finger presses, in bottom screen pixels (320 x 240)\n"
            "anchor_x = 160\n"
            "anchor_y = 120\n"
            "# How far the finger may move from the anchor, in pixels\n"
            "max_travel = 110\n"
            "# Finger speed at full stick deflection, in pixels per second\n"
            "# (higher = faster camera)\n"
            "speed = 400\n"
            "# 1 to invert an axis\n"
            "invert_x = 0\n"
            "invert_y = 0\n"
            "\n"
            "# Show enemies as red dots on the minimap (darker red = another floor)\n"
            "minimap_enemies = 1\n"
            "# Dot size in bottom screen pixels\n"
            "enemy_dot_size = 3\n";
        FileUtil::CreateFullPath(path);
        FileUtil::WriteStringToFile(true, path, defaults);
        LOG_INFO(Service_HID, "Mercs3D: wrote default settings to {}", path);
        return;
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
        if (value.empty()) {
            continue;
        }
        float number = 0.0f;
        try {
            number = std::stof(value);
        } catch (...) {
            LOG_WARNING(Service_HID, "Mercs3D: bad value for {}: {}", key, value);
            continue;
        }
        if (key == "right_stick_camera") {
            mercs3d_config.enabled = number != 0.0f;
        } else if (key == "deadzone") {
            mercs3d_config.deadzone = std::clamp(number, 0.0f, 0.9f);
        } else if (key == "anchor_x") {
            mercs3d_config.anchor_x =
                std::clamp(number, 0.0f, static_cast<float>(Core::kScreenBottomWidth - 1));
        } else if (key == "anchor_y") {
            mercs3d_config.anchor_y =
                std::clamp(number, 0.0f, static_cast<float>(Core::kScreenBottomHeight - 1));
        } else if (key == "max_travel") {
            mercs3d_config.max_travel = std::max(number, 0.0f);
        } else if (key == "speed") {
            mercs3d_config.speed = std::max(number, 0.0f);
        } else if (key == "invert_x") {
            mercs3d_config.invert_x = number != 0.0f;
        } else if (key == "invert_y") {
            mercs3d_config.invert_y = number != 0.0f;
        }
    }
    LOG_INFO(Service_HID, "Mercs3D: loaded settings from {}", path);
}

bool Module::IsMercs3DRunning() const {
    // Resident Evil: The Mercenaries 3D (USA) (Rev 1)
    constexpr u64 MERCS3D_TITLE_ID = 0x0004000000035900ULL;
    for (const auto& p : system.Kernel().GetProcessList()) {
        if (p && p->codeset && p->codeset->program_id == MERCS3D_TITLE_ID) {
            return true;
        }
    }
    return false;
}

bool Module::UpdateMercs3DRightStickTouch(u16& touch_x, u16& touch_y) {
    // The game's look-around camera is driven by dragging on the minimap (bottom screen).
    // Turn the right stick (mapped to the C-Stick) into a virtual finger dragging there:
    // the finger moves at a speed proportional to stick deflection, and lifts (camera snaps
    // back) when the stick returns to center.
    const auto& cfg = mercs3d_config;
    const float DEADZONE = cfg.deadzone;
    const float ANCHOR_X = cfg.anchor_x;
    const float ANCHOR_Y = cfg.anchor_y;
    const float MAX_TRAVEL = cfg.max_travel;
    const float SPEED_PX_PER_SEC = cfg.speed;
    // The pad updates 234 times per second.
    constexpr float UPDATES_PER_SEC = 234.0f;

    // While aiming with the RE5 controls the right stick aims instead.
    if (!cfg.enabled || !mercs3d_c_stick || mercs3d_re5_aiming || !IsMercs3DRunning()) {
        mercs3d_finger_down = false;
        return false;
    }

    auto [x, y] = mercs3d_c_stick->GetStatus();
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude < DEADZONE) {
        mercs3d_finger_down = false;
        return false;
    }
    const float scaled = std::min((magnitude - DEADZONE) / (1.0f - DEADZONE), 1.0f);
    x = x / magnitude * scaled;
    y = y / magnitude * scaled;

    if (!mercs3d_finger_down) {
        mercs3d_finger_down = true;
        mercs3d_finger_x = ANCHOR_X;
        mercs3d_finger_y = ANCHOR_Y;
    } else {
        const float step = SPEED_PX_PER_SEC / UPDATES_PER_SEC;
        mercs3d_finger_x += (cfg.invert_x ? -x : x) * step;
        // By default, stick up = finger moves up the screen.
        mercs3d_finger_y -= (cfg.invert_y ? -y : y) * step;
        mercs3d_finger_x =
            std::clamp(mercs3d_finger_x, ANCHOR_X - MAX_TRAVEL, ANCHOR_X + MAX_TRAVEL);
        mercs3d_finger_y =
            std::clamp(mercs3d_finger_y, ANCHOR_Y - MAX_TRAVEL, ANCHOR_Y + MAX_TRAVEL);
        mercs3d_finger_x =
            std::clamp(mercs3d_finger_x, 0.0f, static_cast<float>(Core::kScreenBottomWidth - 1));
        mercs3d_finger_y =
            std::clamp(mercs3d_finger_y, 0.0f, static_cast<float>(Core::kScreenBottomHeight - 1));
    }
    touch_x = static_cast<u16>(mercs3d_finger_x);
    touch_y = static_cast<u16>(mercs3d_finger_y);
    return true;
}

void Module::UpdatePadCallback(std::uintptr_t user_data, s64 cycles_late) {
    SharedMem* mem = reinterpret_cast<SharedMem*>(shared_mem->GetPointer());

    if (is_device_reload_pending.exchange(false))
        LoadInputDevices();

    // Mercenaries 3D: starting point menu while a mission loads (blocks until answered).
    Mercs3D::UpdateSpawnMenu(system, [this] {
        using namespace Settings::NativeButton;
        PadState pad{};
        if (!buttons[A - BUTTON_HID_BEGIN]) {
            return u32{0};
        }
        pad.a.Assign(buttons[A - BUTTON_HID_BEGIN]->GetStatus());
        pad.y.Assign(buttons[Y - BUTTON_HID_BEGIN]->GetStatus());
        pad.left.Assign(buttons[Left - BUTTON_HID_BEGIN]->GetStatus());
        pad.right.Assign(buttons[Right - BUTTON_HID_BEGIN]->GetStatus());
        const float x = std::get<0>(circle_pad->GetStatus());
        pad.circle_left.Assign(x < -0.5f);
        pad.circle_right.Assign(x > 0.5f);
        return pad.hex;
    });

    using namespace Settings::NativeButton;

    if (artic_controller.get() && artic_controller->IsReady()) {
        constexpr u32 HID_VALID_KEYS = 0xF0003FFF;
        constexpr u32 LIBCTRU_TOUCH_KEY = (1 << 20);

        ArticBaseController::ControllerData data = artic_controller->GetControllerData();

        state.hex = data.pad & HID_VALID_KEYS;

        s16 circle_pad_x = data.c_pad_x;
        s16 circle_pad_y = data.c_pad_y;

        system.Movie().HandlePadAndCircleStatus(state, circle_pad_x, circle_pad_y);

        mem->pad.current_state.hex = state.hex;
        mem->pad.index = next_pad_index;
        next_pad_index = (next_pad_index + 1) % mem->pad.entries.size();

        // Get the previous Pad state
        u32 last_entry_index = (mem->pad.index - 1) % mem->pad.entries.size();
        PadState old_state = mem->pad.entries[last_entry_index].current_state;

        // Compute bitmask with 1s for bits different from the old state
        PadState changed = {{(state.hex ^ old_state.hex)}};

        // Get the current Pad entry
        PadDataEntry& pad_entry = mem->pad.entries[mem->pad.index];

        // Update entry properties
        pad_entry.current_state.hex = state.hex;
        pad_entry.delta_additions.hex = changed.hex & state.hex;
        pad_entry.delta_removals.hex = changed.hex & old_state.hex;
        pad_entry.circle_pad_x = circle_pad_x;
        pad_entry.circle_pad_y = circle_pad_y;

        // If we just updated index 0, provide a new timestamp
        if (mem->pad.index == 0) {
            mem->pad.index_reset_ticks_previous = mem->pad.index_reset_ticks;
            mem->pad.index_reset_ticks = (s64)system.CoreTiming().GetTicks();
        }

        mem->touch.index = next_touch_index;
        next_touch_index = (next_touch_index + 1) % mem->touch.entries.size();

        // Get the current touch entry
        TouchDataEntry& touch_entry = mem->touch.entries[mem->touch.index];
        bool pressed = (data.pad & LIBCTRU_TOUCH_KEY) != 0;

        touch_entry.x = static_cast<u16>(data.touch_x);
        touch_entry.y = static_cast<u16>(data.touch_y);
        touch_entry.valid.Assign(pressed ? 1 : 0);

        system.Movie().HandleTouchStatus(touch_entry);
    } else {
        state.a.Assign(buttons[A - BUTTON_HID_BEGIN]->GetStatus());
        state.b.Assign(buttons[B - BUTTON_HID_BEGIN]->GetStatus());
        state.x.Assign(buttons[X - BUTTON_HID_BEGIN]->GetStatus());
        state.y.Assign(buttons[Y - BUTTON_HID_BEGIN]->GetStatus());
        state.right.Assign(buttons[Right - BUTTON_HID_BEGIN]->GetStatus());
        state.left.Assign(buttons[Left - BUTTON_HID_BEGIN]->GetStatus());
        state.up.Assign(buttons[Up - BUTTON_HID_BEGIN]->GetStatus());
        state.down.Assign(buttons[Down - BUTTON_HID_BEGIN]->GetStatus());
        state.l.Assign(buttons[L - BUTTON_HID_BEGIN]->GetStatus());
        state.r.Assign(buttons[R - BUTTON_HID_BEGIN]->GetStatus());
        state.start.Assign(buttons[Start - BUTTON_HID_BEGIN]->GetStatus());
        state.select.Assign(buttons[Select - BUTTON_HID_BEGIN]->GetStatus());
        state.debug.Assign(buttons[Debug - BUTTON_HID_BEGIN]->GetStatus());
        state.gpio14.Assign(buttons[Gpio14 - BUTTON_HID_BEGIN]->GetStatus());

        const InputOverride input = GetInputOverride();
        state.hex |= input.buttons;

        // Mercenaries 3D, controller type B (RE5 style; Mercs 3D settings page, off by default).
        // The game only knows the 3DS buttons, so: ZL aims (= R), ZR fires while aiming (= Y;
        // Y alone stays the action / melee prompt). While aiming, the right stick (C-Stick)
        // aims instead of the Circle Pad; otherwise it keeps driving the look-around camera.
        mercs3d_re5_aiming = false;
        if (IsMercs3DRunning() && Mercs3D::GetLayoutSettings().controller_type_b) {
            if (mercs3d_zl && mercs3d_zl->GetStatus()) {
                state.r.Assign(1);
            }
            mercs3d_re5_aiming = state.r != 0;
            if (mercs3d_re5_aiming && mercs3d_zr && mercs3d_zr->GetStatus()) {
                state.y.Assign(1);
            }
        }

        // Get current circle pad position and update circle pad direction
        float circle_pad_x_f, circle_pad_y_f;
        std::tie(circle_pad_x_f, circle_pad_y_f) = circle_pad->GetStatus();
        if (mercs3d_re5_aiming && mercs3d_c_stick) {
            std::tie(circle_pad_x_f, circle_pad_y_f) = mercs3d_c_stick->GetStatus();
        }

        // xperia64: 0x9A seems to be the calibrated limit of the circle pad
        // Verified by using Input Redirector with very large-value digital inputs
        // on the circle pad and calibrating using the system settings application
        constexpr int MAX_CIRCLEPAD_POS = 0x9A; // Max value for a circle pad position

        // These are rounded rather than truncated on actual hardware
        s16 circle_pad_new_x = static_cast<s16>(std::roundf(circle_pad_x_f * MAX_CIRCLEPAD_POS));
        s16 circle_pad_new_y = static_cast<s16>(std::roundf(circle_pad_y_f * MAX_CIRCLEPAD_POS));
        if (input.circle_pad_set) {
            circle_pad_new_x =
                std::clamp<s16>(input.circle_pad_x, -MAX_CIRCLEPAD_POS, MAX_CIRCLEPAD_POS);
            circle_pad_new_y =
                std::clamp<s16>(input.circle_pad_y, -MAX_CIRCLEPAD_POS, MAX_CIRCLEPAD_POS);
        }
        s16 circle_pad_x = (circle_pad_new_x +
                            std::accumulate(circle_pad_old_x.begin(), circle_pad_old_x.end(), 0)) /
                           CIRCLE_PAD_AVERAGING;
        s16 circle_pad_y = (circle_pad_new_y +
                            std::accumulate(circle_pad_old_y.begin(), circle_pad_old_y.end(), 0)) /
                           CIRCLE_PAD_AVERAGING;
        circle_pad_old_x.erase(circle_pad_old_x.begin());
        circle_pad_old_x.push_back(circle_pad_new_x);
        circle_pad_old_y.erase(circle_pad_old_y.begin());
        circle_pad_old_y.push_back(circle_pad_new_y);

        system.Movie().HandlePadAndCircleStatus(state, circle_pad_x, circle_pad_y);

        const DirectionState direction = GetStickDirectionState(circle_pad_x, circle_pad_y);
        state.circle_up.Assign(direction.up);
        state.circle_down.Assign(direction.down);
        state.circle_left.Assign(direction.left);
        state.circle_right.Assign(direction.right);

        mem->pad.current_state.hex = state.hex;
        mem->pad.index = next_pad_index;
        next_pad_index = (next_pad_index + 1) % mem->pad.entries.size();

        // Get the previous Pad state
        u32 last_entry_index = (mem->pad.index - 1) % mem->pad.entries.size();
        PadState old_state = mem->pad.entries[last_entry_index].current_state;

        // Compute bitmask with 1s for bits different from the old state
        PadState changed = {{(state.hex ^ old_state.hex)}};

        // Get the current Pad entry
        PadDataEntry& pad_entry = mem->pad.entries[mem->pad.index];

        // Update entry properties
        pad_entry.current_state.hex = state.hex;
        pad_entry.delta_additions.hex = changed.hex & state.hex;
        pad_entry.delta_removals.hex = changed.hex & old_state.hex;
        pad_entry.circle_pad_x = circle_pad_x;
        pad_entry.circle_pad_y = circle_pad_y;

        // If we just updated index 0, provide a new timestamp
        if (mem->pad.index == 0) {
            mem->pad.index_reset_ticks_previous = mem->pad.index_reset_ticks;
            mem->pad.index_reset_ticks = (s64)system.CoreTiming().GetTicks();
        }

        mem->touch.index = next_touch_index;
        next_touch_index = (next_touch_index + 1) % mem->touch.entries.size();

        // Get the current touch entry
        TouchDataEntry& touch_entry = mem->touch.entries[mem->touch.index];
        bool pressed = false;
        float x, y;
        std::tie(x, y, pressed) = touch_device->GetStatus();
        if (!pressed && touch_btn_device) {
            std::tie(x, y, pressed) = touch_btn_device->GetStatus();
        }
        if (!pressed && controller_touch_device) {
            std::tie(x, y, pressed) = controller_touch_device->GetStatus();
        }
        touch_entry.x = static_cast<u16>(x * Core::kScreenBottomWidth);
        touch_entry.y = static_cast<u16>(y * Core::kScreenBottomHeight);
        if (!pressed && input.touch_pressed) {
            pressed = true;
            touch_entry.x = std::min<u16>(input.touch_x, Core::kScreenBottomWidth - 1);
            touch_entry.y = std::min<u16>(input.touch_y, Core::kScreenBottomHeight - 1);
        }
        if (!pressed) {
            u16 virtual_x, virtual_y;
            if (UpdateMercs3DRightStickTouch(virtual_x, virtual_y)) {
                pressed = true;
                touch_entry.x = virtual_x;
                touch_entry.y = virtual_y;
            }
        } else {
            // A real touch always wins; drop the virtual finger so it restarts cleanly.
            mercs3d_finger_down = false;
        }
        touch_entry.valid.Assign(pressed ? 1 : 0);

        system.Movie().HandleTouchStatus(touch_entry);
    }

    // TODO(bunnei): We're not doing anything with offset 0xA8 + 0x18 of HID SharedMemory, which
    // supposedly is "Touch-screen entry, which contains the raw coordinate data prior to being
    // converted to pixel coordinates." (http://3dbrew.org/wiki/HID_Shared_Memory#Offset_0xA8).

    // If we just updated index 0, provide a new timestamp
    if (mem->touch.index == 0) {
        mem->touch.index_reset_ticks_previous = mem->touch.index_reset_ticks;
        mem->touch.index_reset_ticks = (s64)system.CoreTiming().GetTicks();
    }

    // Signal both handles when there's an update to Pad or touch
    event_pad_or_touch_1->Signal();
    event_pad_or_touch_2->Signal();

    // TODO(xperia64): How the 3D Slider is updated by the HID module needs to be RE'd
    // and possibly moved to its own Core::Timing event.
    mem->pad.sliderstate_3d = (Settings::values.factor_3d.GetValue() / 100.0f);
    system.Kernel().GetSharedPageHandler().Set3DSlider(Settings::values.factor_3d.GetValue() /
                                                       100.0f);

    // Reschedule recurrent event
    system.CoreTiming().ScheduleEvent(pad_update_ticks - cycles_late, pad_update_event);
}

void Module::RestartPadUpdates() {
    // Do not change the timings when movies are involved.
    const auto play_mode = system.Movie().GetPlayMode();
    if (play_mode == Core::Movie::PlayMode::Recording ||
        play_mode == Core::Movie::PlayMode::Playing) {
        return;
    }
    system.CoreTiming().UnscheduleEvent(pad_update_event, 0);
    UpdatePadCallback(0, 0);
}

void Module::UpdateAccelerometerCallback(std::uintptr_t user_data, s64 cycles_late) {
    SharedMem* mem = reinterpret_cast<SharedMem*>(shared_mem->GetPointer());

    mem->accelerometer.index = next_accelerometer_index;
    next_accelerometer_index = (next_accelerometer_index + 1) % mem->accelerometer.entries.size();

    AccelerometerDataEntry& accelerometer_entry =
        mem->accelerometer.entries[mem->accelerometer.index];

    if (artic_controller.get() && artic_controller->IsReady()) {
        ArticBaseController::ControllerData data = artic_controller->GetControllerData();

        accelerometer_entry.x = data.accel_x;
        accelerometer_entry.y = data.accel_y;
        accelerometer_entry.z = data.accel_z;
    } else {
        Common::Vec3<float> accel;
        std::tie(accel, std::ignore) = motion_device->GetStatus();
        accel *= accelerometer_coef;
        // TODO(wwylele): do a time stretch like the one in UpdateGyroscopeCallback
        // The time stretch formula should be like
        // stretched_vector = (raw_vector - gravity) * stretch_ratio + gravity

        accelerometer_entry.x = static_cast<s16>(accel.x);
        accelerometer_entry.y = static_cast<s16>(accel.y);
        accelerometer_entry.z = static_cast<s16>(accel.z);
    }

    system.Movie().HandleAccelerometerStatus(accelerometer_entry);

    // Make up "raw" entry
    // TODO(wwylele):
    // From hardware testing, the raw_entry values are approximately, but not exactly, as twice as
    // corresponding entries (or with a minus sign). It may caused by system calibration to the
    // accelerometer. Figure out how it works, or, if no game reads raw_entry, the following three
    // lines can be removed and leave raw_entry unimplemented.
    mem->accelerometer.raw_entry.x = -2 * accelerometer_entry.x;
    mem->accelerometer.raw_entry.z = 2 * accelerometer_entry.y;
    mem->accelerometer.raw_entry.y = -2 * accelerometer_entry.z;

    // If we just updated index 0, provide a new timestamp
    if (mem->accelerometer.index == 0) {
        mem->accelerometer.index_reset_ticks_previous = mem->accelerometer.index_reset_ticks;
        mem->accelerometer.index_reset_ticks = (s64)system.CoreTiming().GetTicks();
    }

    event_accelerometer->Signal();

    // Reschedule recurrent event
    system.CoreTiming().ScheduleEvent(accelerometer_update_ticks - cycles_late,
                                      accelerometer_update_event);
}

void Module::UpdateGyroscopeCallback(std::uintptr_t user_data, s64 cycles_late) {
    SharedMem* mem = reinterpret_cast<SharedMem*>(shared_mem->GetPointer());

    mem->gyroscope.index = next_gyroscope_index;
    next_gyroscope_index = (next_gyroscope_index + 1) % mem->gyroscope.entries.size();

    GyroscopeDataEntry& gyroscope_entry = mem->gyroscope.entries[mem->gyroscope.index];

    if (artic_controller.get() && artic_controller->IsReady()) {
        ArticBaseController::ControllerData data = artic_controller->GetControllerData();

        gyroscope_entry.x = data.gyro_x;
        gyroscope_entry.y = data.gyro_y;
        gyroscope_entry.z = data.gyro_z;
    } else {
        Common::Vec3<float> gyro;
        std::tie(std::ignore, gyro) = motion_device->GetStatus();
        double stretch = system.perf_stats->GetLastFrameTimeScale();
        gyro *= gyroscope_coef * static_cast<float>(stretch);
        gyroscope_entry.x = static_cast<s16>(gyro.x);
        gyroscope_entry.y = static_cast<s16>(gyro.y);
        gyroscope_entry.z = static_cast<s16>(gyro.z);
    }

    system.Movie().HandleGyroscopeStatus(gyroscope_entry);

    // Make up "raw" entry
    mem->gyroscope.raw_entry.x = gyroscope_entry.x;
    mem->gyroscope.raw_entry.z = -gyroscope_entry.y;
    mem->gyroscope.raw_entry.y = gyroscope_entry.z;

    // If we just updated index 0, provide a new timestamp
    if (mem->gyroscope.index == 0) {
        mem->gyroscope.index_reset_ticks_previous = mem->gyroscope.index_reset_ticks;
        mem->gyroscope.index_reset_ticks = (s64)system.CoreTiming().GetTicks();
    }

    event_gyroscope->Signal();

    // Reschedule recurrent event
    system.CoreTiming().ScheduleEvent(gyroscope_update_ticks - cycles_late, gyroscope_update_event);
}

void Module::Interface::GetIPCHandles(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    IPC::RequestBuilder rb = rp.MakeBuilder(1, 7);
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(hid->shared_mem, hid->event_pad_or_touch_1, hid->event_pad_or_touch_2,
                       hid->event_accelerometer, hid->event_gyroscope, hid->event_debug_pad);
}

void Module::Interface::EnableAccelerometer(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    auto& artic_client = GetModule()->artic_client;
    if (artic_client.get()) {
        auto req = artic_client->NewRequest("HIDUSER_EnableAccelerometer");

        auto resp = artic_client->Send(req);

        if (!resp.has_value()) {
            rb.Push(ResultUnknown);
        } else {
            rb.Push(Result{static_cast<u32>(resp->GetMethodResult())});
        }
    } else {
        rb.Push(ResultSuccess);
    }

    ++hid->enable_accelerometer_count;

    // Schedules the accelerometer update event if the accelerometer was just enabled
    if (hid->enable_accelerometer_count == 1) {
        hid->system.CoreTiming().ScheduleEvent(accelerometer_update_ticks,
                                               hid->accelerometer_update_event);
    }

    LOG_DEBUG(Service_HID, "called");
}

void Module::Interface::DisableAccelerometer(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    auto& artic_client = GetModule()->artic_client;
    if (artic_client.get()) {
        auto req = artic_client->NewRequest("HIDUSER_DisableAccelerometer");

        auto resp = artic_client->Send(req);

        if (!resp.has_value()) {
            rb.Push(ResultUnknown);
        } else {
            rb.Push(Result{static_cast<u32>(resp->GetMethodResult())});
        }
    } else {
        rb.Push(ResultSuccess);
    }

    --hid->enable_accelerometer_count;

    // Unschedules the accelerometer update event if the accelerometer was just disabled
    if (hid->enable_accelerometer_count == 0) {
        hid->system.CoreTiming().UnscheduleEvent(hid->accelerometer_update_event, 0);
    }

    LOG_DEBUG(Service_HID, "called");
}

void Module::Interface::EnableGyroscopeLow(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    auto& artic_client = GetModule()->artic_client;
    if (artic_client.get()) {
        auto req = artic_client->NewRequest("HIDUSER_EnableGyroscope");

        auto resp = artic_client->Send(req);

        if (!resp.has_value()) {
            rb.Push(ResultUnknown);
        } else {
            rb.Push(Result{static_cast<u32>(resp->GetMethodResult())});
        }
    } else {
        rb.Push(ResultSuccess);
    }

    ++hid->enable_gyroscope_count;

    // Schedules the gyroscope update event if the gyroscope was just enabled
    if (hid->enable_gyroscope_count == 1) {
        hid->system.CoreTiming().ScheduleEvent(gyroscope_update_ticks, hid->gyroscope_update_event);
    }

    LOG_DEBUG(Service_HID, "called");
}

void Module::Interface::DisableGyroscopeLow(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    auto& artic_client = GetModule()->artic_client;
    if (artic_client.get()) {
        auto req = artic_client->NewRequest("HIDUSER_DisableGyroscope");

        auto resp = artic_client->Send(req);

        if (!resp.has_value()) {
            rb.Push(ResultUnknown);
        } else {
            rb.Push(Result{static_cast<u32>(resp->GetMethodResult())});
        }
    } else {
        rb.Push(ResultSuccess);
    }

    --hid->enable_gyroscope_count;

    // Unschedules the gyroscope update event if the gyroscope was just disabled
    if (hid->enable_gyroscope_count == 0) {
        hid->system.CoreTiming().UnscheduleEvent(hid->gyroscope_update_event, 0);
    }

    LOG_DEBUG(Service_HID, "called");
}

void Module::Interface::GetGyroscopeLowRawToDpsCoefficient(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);

    auto& artic_client = GetModule()->artic_client;
    if (artic_client.get()) {
        auto req = artic_client->NewRequest("HIDUSER_GetGyroRawToDpsCoef");

        auto resp = artic_client->Send(req);

        if (!resp.has_value()) {
            rb.Push(ResultUnknown);
            rb.Push(0.f);
            return;
        }

        Result res = Result{static_cast<u32>(resp->GetMethodResult())};
        if (res.IsError()) {
            rb.Push(res);
            rb.Push(0.f);
            return;
        }

        auto coef = resp->GetResponseFloat(0);
        if (!coef.has_value()) {
            rb.Push(ResultUnknown);
            rb.Push(0.f);
            return;
        }

        rb.Push(res);
        rb.Push(*coef);
    } else {
        rb.Push(ResultSuccess);
        rb.Push(gyroscope_coef);
    }
}

void Module::Interface::GetGyroscopeLowCalibrateParam(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    IPC::RequestBuilder rb = rp.MakeBuilder(6, 0);

    auto& artic_client = GetModule()->artic_client;
    if (artic_client.get()) {
        GyroscopeCalibrateParam param;

        auto req = artic_client->NewRequest("HIDUSER_GetGyroCalibrateParam");

        auto resp = artic_client->Send(req);

        if (!resp.has_value()) {
            rb.Push(ResultUnknown);
            rb.PushRaw(param);
            return;
        }

        Result res = Result{static_cast<u32>(resp->GetMethodResult())};
        if (res.IsError()) {
            rb.Push(res);
            rb.PushRaw(param);
            return;
        }

        auto param_buf = resp->GetResponseBuffer(0);
        if (!param_buf.has_value() || param_buf->second != sizeof(param)) {
            rb.Push(ResultUnknown);
            rb.PushRaw(param);
            return;
        }
        memcpy(&param, param_buf->first, sizeof(param));

        rb.Push(res);
        rb.PushRaw(param);
    } else {
        rb.Push(ResultSuccess);

        const s16 param_unit = 6700; // an approximate value taken from hw
        GyroscopeCalibrateParam param = {
            {0, param_unit, -param_unit},
            {0, param_unit, -param_unit},
            {0, param_unit, -param_unit},
        };
        rb.PushRaw(param);

        LOG_WARNING(Service_HID, "(STUBBED) called");
    }
}

void Module::Interface::GetSoundVolume(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);

    const u8 volume = static_cast<u8>(0x3F * Settings::values.volume.GetValue());

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(ResultSuccess);
    rb.Push(volume);
}

Module::Interface::Interface(std::shared_ptr<Module> hid, const char* name, u32 max_session)
    : ServiceFramework(name, max_session), hid(std::move(hid)) {}

std::shared_ptr<Module> Module::Interface::GetModule() const {
    return hid;
}

Module::Module(Core::System& system) : system(system) {
    using namespace Kernel;

    shared_mem =
        system.Kernel()
            .CreateSharedMemory(nullptr, 0x1000, MemoryPermission::ReadWrite,
                                MemoryPermission::Read, 0, MemoryRegion::BASE, "HID:SharedMemory")
            .Unwrap();

    // Create event handles
    event_pad_or_touch_1 = system.Kernel().CreateEvent(ResetType::OneShot, "HID:EventPadOrTouch1");
    event_pad_or_touch_2 = system.Kernel().CreateEvent(ResetType::OneShot, "HID:EventPadOrTouch2");
    event_accelerometer = system.Kernel().CreateEvent(ResetType::OneShot, "HID:EventAccelerometer");
    event_gyroscope = system.Kernel().CreateEvent(ResetType::OneShot, "HID:EventGyroscope");
    event_debug_pad = system.Kernel().CreateEvent(ResetType::OneShot, "HID:EventDebugPad");

    // Register update callbacks
    Core::Timing& timing = system.CoreTiming();
    pad_update_event = timing.RegisterEvent("HID::UpdatePadCallback",
                                            [this](std::uintptr_t user_data, s64 cycles_late) {
                                                UpdatePadCallback(user_data, cycles_late);
                                            });
    accelerometer_update_event = timing.RegisterEvent(
        "HID::UpdateAccelerometerCallback", [this](std::uintptr_t user_data, s64 cycles_late) {
            UpdateAccelerometerCallback(user_data, cycles_late);
        });
    gyroscope_update_event = timing.RegisterEvent(
        "HID::UpdateGyroscopeCallback", [this](std::uintptr_t user_data, s64 cycles_late) {
            UpdateGyroscopeCallback(user_data, cycles_late);
        });

    timing.ScheduleEvent(pad_update_ticks, pad_update_event);
}

void Module::UseArticClient(const std::shared_ptr<Network::ArticBase::Client>& client) {
    artic_client = client;
    artic_controller = std::make_shared<ArticBaseController>(client);
    if (!artic_controller->IsCreated()) {
        artic_controller.reset();
    } else {
        auto ir_user = system.ServiceManager().GetService<Service::IR::IR_USER>("ir:USER");
        if (ir_user.get()) {
            ir_user->UseArticController(artic_controller);
        }

        auto ir_rst = system.ServiceManager().GetService<Service::IR::IR_RST>("ir:rst");
        if (ir_rst.get()) {
            ir_rst->UseArticController(artic_controller);
        }
    }
}

void Module::ReloadInputDevices() {
    is_device_reload_pending.store(true);
}

const PadState& Module::GetState() const {
    return state;
}

void Module::SetInputOverride(const InputOverride& input) {
    std::scoped_lock lock{input_override_mutex};
    input_override = input;
}

Module::InputOverride Module::GetInputOverride() const {
    std::scoped_lock lock{input_override_mutex};
    return input_override;
}

std::shared_ptr<Module> GetModule(Core::System& system) {
    auto hid = system.ServiceManager().GetService<Service::HID::Module::Interface>("hid:USER");
    if (!hid)
        return nullptr;
    return hid->GetModule();
}

void InstallInterfaces(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    auto hid = std::make_shared<Module>(system);
    std::make_shared<User>(hid)->InstallAsService(service_manager);
    std::make_shared<Spvr>(hid)->InstallAsService(service_manager);
}

} // namespace Service::HID
