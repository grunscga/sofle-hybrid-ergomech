/*
 * Copyright (c) 2026 Greg Grunschel
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_mouse_speed

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <mouse_speed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int on_mouse_speed_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    mouse_speed_adjust(binding->param1 == 0 ? -1 : 1);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_mouse_speed_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api mouse_speed_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_mouse_speed_pressed,
    .binding_released = on_mouse_speed_released,
};

#define MOUSE_SPEED_INST(n)                                                                        \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mouse_speed_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MOUSE_SPEED_INST)
