/*
 * Copyright (c) 2026 Greg Grunschel
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Must match ZMK_POINTING_DEFAULT_MOVE_VAL in the keymap. The hat still
 * ramps to this value; emitted counts are scaled by speed / this base. */
#define MOUSE_SPEED_BASE 1800

/* Eleven rungs, (3600 - 90) / 10 apart. Index 2 is the untouched default. */
#define MOUSE_SPEED_RUNG_COUNT 11
#define MOUSE_SPEED_DEFAULT_RUNG 2

/* Five host profiles. Not ZMK_BLE_PROFILE_COUNT: that macro is smaller
 * on the peripheral, and the stored blob must stay the same size. */
#define MOUSE_SPEED_BLE_SLOTS 5

uint16_t mouse_speed_current(void);

/* direction < 0 steps toward the minimum. Any other value steps up.
 * Clamps at either end. Returns the speed now in effect, in counts/sec. */
uint16_t mouse_speed_adjust(int direction);
