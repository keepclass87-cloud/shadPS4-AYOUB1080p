// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <mutex>

#include "common/assert.h"
#include "common/types.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "input_mouse.h"

#include <common/singleton.h>
#include <emulator.h>
#include "SDL3/SDL.h"

extern Frontend::WindowSDL* g_window;

namespace Input {

extern std::list<std::pair<InputEvent, bool>> pressed_keys;

int mouse_joystick_binding = 0;
float mouse_deadzone_offset = 0.5, mouse_speed = 1, mouse_speed_offset = 0.1250;
bool mouse_gyro_roll_mode = false;
Uint32 mouse_polling_id = 0;
MouseMode mouse_mode = MouseMode::Off;

// Switches mouse to a set mode or turns mouse emulation off if it was already in that mode.
// Returns whether the mode is turned on.
bool ToggleMouseModeTo(MouseMode m) {
    if (mouse_mode == m) {
        mouse_mode = MouseMode::Off;
    } else {
        mouse_mode = m;
    }
    return mouse_mode == m;
}

void SetMouseToJoystick(int joystick) {
    mouse_joystick_binding = joystick;
}

void SetMouseParams(float mdo, float ms, float mso) {
    mouse_deadzone_offset = mdo;
    mouse_speed = ms;
    mouse_speed_offset = mso;
}

void SetMouseGyroRollMode(bool mode) {
    mouse_gyro_roll_mode = mode;
}

void EmulateJoystick(GameController* controller, u32 interval) {

    Axis axis_x, axis_y;
    switch (mouse_joystick_binding) {
    case 1:
        axis_x = Axis::LeftX;
        axis_y = Axis::LeftY;
        break;
    case 2:
        axis_x = Axis::RightX;
        axis_y = Axis::RightY;
        break;
    default:
        return; // no update needed
    }

    float d_x = 0, d_y = 0;
    SDL_GetRelativeMouseState(&d_x, &d_y);

    float output_speed =
        SDL_clamp(sqrt(d_x * d_x + d_y * d_y) * mouse_speed + mouse_speed_offset * 128,
                  mouse_deadzone_offset * 128, 128.0);

    float angle = atan2(d_y, d_x);
    float a_x = cos(angle) * output_speed, a_y = sin(angle) * output_speed;

    if (d_x != 0 || d_y != 0) {
        controller->Axis(axis_x, GetAxis(-0x80, 0x7f, a_x), false);
        controller->Axis(axis_y, GetAxis(-0x80, 0x7f, a_y), false);
    } else {
        controller->Axis(axis_x, GetAxis(-0x80, 0x7f, 0), false);
        controller->Axis(axis_y, GetAxis(-0x80, 0x7f, 0), false);
    }
}

constexpr float constant_down_accel[3] = {0.0f, 9.81f, 0.0f};
void EmulateGyro(GameController* controller, u32 interval) {
    float d_x = 0, d_y = 0;
    SDL_GetRelativeMouseState(&d_x, &d_y);
    controller->UpdateAcceleration(constant_down_accel);
    float gyro_from_mouse[3] = {-d_y / 100, -d_x / 100, 0.0f};
    if (mouse_gyro_roll_mode) {
        gyro_from_mouse[1] = 0.0f;
        gyro_from_mouse[2] = -d_x / 100;
    }
    controller->UpdateGyro(gyro_from_mouse);
}

void EmulateTouchpad(GameController* controller, u32 interval) {
    float x, y;
    SDL_MouseButtonFlags mouse_buttons = SDL_GetMouseState(&x, &y);
    controller->SetTouchpadState(0, (mouse_buttons & SDL_BUTTON_LMASK) != 0,
                                 std::clamp(x / g_window->GetWidth(), 0.0f, 1.0f),
                                 std::clamp(y / g_window->GetHeight(), 0.0f, 1.0f));
    controller->Button(Libraries::Pad::OrbisPadButtonDataOffset::TouchPad,
                       (mouse_buttons & SDL_BUTTON_RMASK) != 0);
}

void ApplyMouseInputBlockers() {
    switch (mouse_mode) {
    case MouseMode::Touchpad:
        for (auto& k : pressed_keys) {
            if (k.first.input.sdl_id == SDL_BUTTON_LEFT ||
                k.first.input.sdl_id == SDL_BUTTON_RIGHT) {
                k.second = true;
            }
        }
        break;
    default:
        break;
    }
}

namespace {

// Number of polling ticks (33 ms each) the finger travels during a swipe.
constexpr int swipe_move_ticks = 6;

struct SwipeState {
    GameController* controller = nullptr;
    float from_x = 0, from_y = 0, to_x = 0, to_y = 0;
    int tick = -1; // -1: no swipe in progress
};

struct ShakeState {
    GameController* controller = nullptr;
    bool active = false;
    bool was_active = false;
    u32 tick = 0;
};

std::mutex gesture_mutex;
SwipeState swipe_state;
ShakeState shake_state;

// Advances the swipe by one tick. Returns true while the swipe owns touch slot 0.
bool UpdateTouchpadSwipe() {
    std::lock_guard lock{gesture_mutex};
    auto& s = swipe_state;
    if (s.tick < 0 || s.controller == nullptr) {
        return false;
    }
    if (s.tick > swipe_move_ticks) {
        // lift the finger
        s.controller->SetTouchpadState(0, false, s.to_x, s.to_y);
        s.tick = -1;
        return true;
    }
    const float t = static_cast<float>(s.tick) / swipe_move_ticks;
    s.controller->SetTouchpadState(0, true, s.from_x + (s.to_x - s.from_x) * t,
                                   s.from_y + (s.to_y - s.from_y) * t);
    s.tick++;
    return true;
}

void UpdateMotionShake() {
    std::lock_guard lock{gesture_mutex};
    auto& s = shake_state;
    if (s.controller == nullptr) {
        return;
    }
    if (s.active) {
        // Alternate strong up/down and sideways jolts about 5 times a second (3 polling ticks
        // per direction), like rattling a spray can.
        const float sign = ((s.tick / 3) % 2 == 0) ? 1.0f : -1.0f;
        const float accel[3] = {sign * 12.0f, 9.81f + sign * 25.0f, sign * -8.0f};
        const float gyro[3] = {sign * 6.0f, 0.0f, sign * -4.0f};
        s.controller->UpdateMotion(gyro, accel);
        s.tick++;
        s.was_active = true;
    } else if (s.was_active) {
        const float accel[3] = {0.0f, 9.81f, 0.0f};
        const float gyro[3] = {0.0f, 0.0f, 0.0f};
        s.controller->UpdateMotion(gyro, accel);
        s.was_active = false;
    }
}

} // namespace

void StartTouchpadSwipe(GameController* controller, float from_x, float from_y, float to_x,
                        float to_y) {
    std::lock_guard lock{gesture_mutex};
    swipe_state = {controller, from_x, from_y, to_x, to_y, 0};
}

void SetMotionShake(GameController* controller, bool active) {
    std::lock_guard lock{gesture_mutex};
    shake_state.controller = controller;
    shake_state.active = active;
    if (active) {
        shake_state.tick = 0;
    }
}

Uint32 MousePolling(void* param, Uint32 id, Uint32 interval) {
    auto* controller = (GameController*)param;
    const bool swipe_running = UpdateTouchpadSwipe();
    switch (mouse_mode) {
    case MouseMode::Joystick:
        EmulateJoystick(controller, interval);
        break;
    case MouseMode::Gyro:
        EmulateGyro(controller, interval);
        break;
    case MouseMode::Touchpad:
        // a keyboard swipe takes over the touchpad until it finishes
        if (!swipe_running) {
            EmulateTouchpad(controller, interval);
        }
        break;

    default:
        break;
    }
    // applied after the mouse modes so that shaking overrides the mouse-gyro accelerometer
    UpdateMotionShake();
    return interval;
}

} // namespace Input
