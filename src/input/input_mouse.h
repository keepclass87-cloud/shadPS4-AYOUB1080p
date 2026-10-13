// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "SDL3/SDL.h"
#include "common/types.h"

namespace Input {

enum MouseMode {
    Off = 0,
    Joystick,
    Gyro,
    Touchpad,
};

bool ToggleMouseModeTo(MouseMode m);
void SetMouseToJoystick(int joystick);
void SetMouseParams(float mouse_deadzone_offset, float mouse_speed, float mouse_speed_offset);
void SetMouseGyroRollMode(bool mode);

void EmulateJoystick(GameController* controller, u32 interval);
void EmulateGyro(GameController* controller, u32 interval);

void ApplyMouseInputBlockers();

// Plays a one-finger swipe on the touchpad from (from_x, from_y) to (to_x, to_y),
// coordinates in 0..1. The finger stays on the touchpad at the end point until
// ReleaseTouchpadSwipe is called (swipe-and-hold). Driven by MousePolling.
void StartTouchpadSwipe(GameController* controller, float from_x, float from_y, float to_x,
                        float to_y);
// Lifts the finger of the current swipe once its movement has finished.
void ReleaseTouchpadSwipe();

// Polls the mouse for changes
Uint32 MousePolling(void* param, Uint32 id, Uint32 interval);

} // namespace Input
