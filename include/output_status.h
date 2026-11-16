/*
 * Copyright (c) 2026 Greg Grunschel
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>

/* Output icon for the left status screen. Shows the active profile number
 * while Bluetooth is selected but not yet connected. */
int sofle_output_status_init(lv_obj_t *parent);
lv_obj_t *sofle_output_status_obj(void);
