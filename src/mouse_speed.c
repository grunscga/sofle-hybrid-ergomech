/*
 * Copyright (c) 2026 Greg Grunschel
 *
 * SPDX-License-Identifier: MIT
 */

#include <mouse_speed.h>

#include <zephyr/kernel.h>

/* 90 + n * (3600 - 90) / 10. Index 2 (792) is the closest rung to 900. */
static const uint16_t speeds[MOUSE_SPEED_RUNG_COUNT] = {
    90, 441, 792, 1143, 1494, 1845, 2196, 2547, 2898, 3249, 3600,
};

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

uint16_t mouse_speed_current(void) { return speeds[MOUSE_SPEED_DEFAULT_RUNG]; }

#else

#include <errno.h>

#include <zephyr/settings/settings.h>

#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_DISPLAY)
#include <lvgl.h>
#include <zmk/display.h>
#endif

struct mouse_speed_settings {
    uint8_t ble[MOUSE_SPEED_BLE_SLOTS];
    uint8_t usb;
} __packed;

static struct mouse_speed_settings slots = {
    .ble =
        {
            MOUSE_SPEED_DEFAULT_RUNG,
            MOUSE_SPEED_DEFAULT_RUNG,
            MOUSE_SPEED_DEFAULT_RUNG,
            MOUSE_SPEED_DEFAULT_RUNG,
            MOUSE_SPEED_DEFAULT_RUNG,
        },
    .usb = MOUSE_SPEED_DEFAULT_RUNG,
};

static uint8_t clamp_rung(uint8_t rung) {
    if (rung >= MOUSE_SPEED_RUNG_COUNT) {
        return MOUSE_SPEED_DEFAULT_RUNG;
    }
    return rung;
}

static int mouse_speed_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                    void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, "rungs", &next) || next) {
        return 0;
    }

    if (len != sizeof(slots)) {
        LOG_WRN("Ignoring mouse speed settings of length %u", (unsigned int)len);
        return 0;
    }

    struct mouse_speed_settings loaded;
    int rc = read_cb(cb_arg, &loaded, sizeof(loaded));
    if (rc < 0) {
        return rc;
    }
    if ((size_t)rc != sizeof(loaded)) {
        return -EINVAL;
    }

    for (int i = 0; i < MOUSE_SPEED_BLE_SLOTS; i++) {
        loaded.ble[i] = clamp_rung(loaded.ble[i]);
    }
    loaded.usb = clamp_rung(loaded.usb);
    slots = loaded;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(mouse_speed, "mouse_speed", NULL, mouse_speed_settings_set, NULL,
                               NULL);

static uint8_t *active_slot(void) {
    struct zmk_endpoint_instance endpoint = zmk_endpoint_get_selected();

    if (endpoint.transport == ZMK_TRANSPORT_USB) {
        return &slots.usb;
    }

    int profile = zmk_ble_active_profile_index();
    if (endpoint.transport == ZMK_TRANSPORT_BLE) {
        profile = endpoint.ble.profile_index;
    }
    if (profile < 0 || profile >= MOUSE_SPEED_BLE_SLOTS) {
        LOG_WRN("Mouse speed profile %d is outside 0..%d", profile, MOUSE_SPEED_BLE_SLOTS - 1);
        profile = 0;
    }
    return &slots.ble[profile];
}

uint16_t mouse_speed_current(void) { return speeds[*active_slot()]; }

static int save_slots(void) {
    int rc = settings_save_one("mouse_speed/rungs", &slots, sizeof(slots));
    if (rc) {
        LOG_ERR("Failed to save mouse speed (%d)", rc);
    }
    return rc;
}

#if IS_ENABLED(CONFIG_ZMK_DISPLAY)

#if defined(LVGL_VERSION_MAJOR) && LVGL_VERSION_MAJOR >= 9
#define ms_scr_act lv_screen_active
#define ms_scr_load lv_screen_load
#else
#define ms_scr_act lv_scr_act
#define ms_scr_load lv_scr_load
#endif

static lv_obj_t *speed_screen;
static lv_obj_t *speed_label;
static lv_obj_t *saved_screen;
static uint16_t shown_speed;

static void hide_speed_screen(struct k_work *work) {
    ARG_UNUSED(work);
    if (saved_screen != NULL) {
        ms_scr_load(saved_screen);
    }
}

K_WORK_DELAYABLE_DEFINE(hide_speed_work, hide_speed_screen);

static void show_speed_screen(struct k_work *work) {
    ARG_UNUSED(work);

    if (!zmk_display_is_initialized()) {
        return;
    }

    if (speed_screen == NULL) {
        speed_screen = lv_obj_create(NULL);
        speed_label = lv_label_create(speed_screen);
        lv_obj_set_style_text_font(speed_label, LV_FONT_DEFAULT, LV_PART_MAIN);
        lv_obj_align(speed_label, LV_ALIGN_CENTER, 0, 0);
    }

    lv_obj_t *active = ms_scr_act();
    if (active != speed_screen) {
        saved_screen = active;
    }

    lv_label_set_text_fmt(speed_label, "%u", (unsigned int)shown_speed);
    ms_scr_load(speed_screen);
    k_work_reschedule_for_queue(zmk_display_work_q(), &hide_speed_work, K_MSEC(1000));
}

K_WORK_DEFINE(show_speed_work, show_speed_screen);

static void queue_speed_screen(uint16_t speed) {
    shown_speed = speed;
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &show_speed_work);
    }
}

#else

static void queue_speed_screen(uint16_t speed) { ARG_UNUSED(speed); }

#endif /* CONFIG_ZMK_DISPLAY */

uint16_t mouse_speed_adjust(int direction) {
    uint8_t *slot = active_slot();
    uint8_t rung = *slot;

    if (direction < 0) {
        if (rung > 0) {
            rung--;
        }
    } else if (rung + 1 < MOUSE_SPEED_RUNG_COUNT) {
        rung++;
    }

    if (rung != *slot) {
        *slot = rung;
        save_slots();
    }

    uint16_t speed = speeds[rung];
    LOG_DBG("Mouse speed %d", speed);
    queue_speed_screen(speed);
    return speed;
}

#endif /* peripheral */
