/*
 * Copyright (c) 2026 Greg Grunschel
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_mouse_speed

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>

#include <drivers/input_processor.h>
#include <mouse_speed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Scale one relative axis. The listener keeps a separate remainder for X and
 * for Y, in the same units as value * speed, so slow rungs still move. */
static void scale_axis(struct input_event *event, struct zmk_input_processor_state *state) {
    int32_t value = (int32_t)event->value * (int32_t)mouse_speed_current();

    if (state != NULL && state->remainder != NULL) {
        value += *state->remainder;
    }

    int32_t scaled = value / MOUSE_SPEED_BASE;

    if (state != NULL && state->remainder != NULL) {
        *state->remainder = (int16_t)(value - (scaled * MOUSE_SPEED_BASE));
    }

    if (scaled > INT16_MAX) {
        scaled = INT16_MAX;
    } else if (scaled < INT16_MIN) {
        scaled = INT16_MIN;
    }

    LOG_DBG("scaled %d to %d", event->value, (int)scaled);
    event->value = (int16_t)scaled;
}

static int mouse_speed_handle_event(const struct device *dev, struct input_event *event,
                                    uint32_t param1, uint32_t param2,
                                    struct zmk_input_processor_state *state) {
    ARG_UNUSED(dev);
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    if (event->code != INPUT_REL_X && event->code != INPUT_REL_Y) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    scale_axis(event, state);
    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api mouse_speed_processor_api = {
    .handle_event = mouse_speed_handle_event,
};

#define MOUSE_SPEED_PROC_INST(n)                                                                   \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mouse_speed_processor_api);

DT_INST_FOREACH_STATUS_OKAY(MOUSE_SPEED_PROC_INST)
