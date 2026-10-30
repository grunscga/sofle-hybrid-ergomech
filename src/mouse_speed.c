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

/* Cover the status screen. Swapping screens leaves the wireless glyph scrambled
 * until a later label update redraws it on its own.
 */
static lv_obj_t *speed_overlay;
static lv_obj_t *speed_label;
static uint16_t shown_speed;

static lv_obj_t *output_status_label(lv_obj_t *screen) {
    lv_obj_t *best = NULL;
    uint32_t count = lv_obj_get_child_count(screen);

    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (!lv_obj_check_type(child, &lv_label_class)) {
            continue;
        }
        if (best == NULL || lv_obj_get_y(child) < lv_obj_get_y(best) ||
            (lv_obj_get_y(child) == lv_obj_get_y(best) && lv_obj_get_x(child) < lv_obj_get_x(best))) {
            best = child;
        }
    }
    return best;
}

static void repair_output_icon(struct k_work *work) {
    ARG_UNUSED(work);

    if (speed_overlay != NULL || !zmk_display_is_initialized()) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *label = screen == NULL ? NULL : output_status_label(screen);
    if (label == NULL) {
        return;
    }

    const char *text = lv_label_get_text(label);
    if (text == NULL) {
        return;
    }

    char copy[32];
    size_t n = 0;
    while (n < sizeof(copy) - 1 && text[n] != '\0') {
        copy[n] = text[n];
        n++;
    }
    copy[n] = '\0';
    lv_label_set_text(label, copy);
}

K_WORK_DEFINE(repair_output_work, repair_output_icon);

static bool repair_armed;
static bool repair_cb_added;

static void on_refr_ready(lv_event_t *event) {
    ARG_UNUSED(event);
    if (!repair_armed) {
        return;
    }
    repair_armed = false;
    k_work_submit_to_queue(zmk_display_work_q(), &repair_output_work);
}

static void hide_speed_screen(struct k_work *work) {
    ARG_UNUSED(work);
    if (speed_overlay == NULL) {
        return;
    }

    lv_obj_delete(speed_overlay);
    speed_overlay = NULL;
    speed_label = NULL;

    /* The refresh that uncovers the status screen can still scramble the wireless
     * glyph. Rewrite that label after the refresh, which is the draw that clears it.
     */
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        return;
    }
    if (!repair_cb_added) {
        lv_display_add_event_cb(disp, on_refr_ready, LV_EVENT_REFR_READY, NULL);
        repair_cb_added = true;
    }
    repair_armed = true;
}

K_WORK_DELAYABLE_DEFINE(hide_speed_work, hide_speed_screen);

static void show_speed_screen(struct k_work *work) {
    ARG_UNUSED(work);

    if (!zmk_display_is_initialized()) {
        return;
    }

    if (speed_overlay == NULL) {
        lv_obj_t *top = lv_layer_top();
        if (top == NULL) {
            return;
        }

        speed_overlay = lv_obj_create(top);
        if (speed_overlay == NULL) {
            return;
        }
        lv_obj_set_size(speed_overlay, lv_display_get_horizontal_resolution(NULL),
                        lv_display_get_vertical_resolution(NULL));
        lv_obj_set_style_radius(speed_overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(speed_overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(speed_overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(speed_overlay, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(speed_overlay, LV_OBJ_FLAG_SCROLLABLE);

        speed_label = lv_label_create(speed_overlay);
        if (speed_label == NULL) {
            lv_obj_delete(speed_overlay);
            speed_overlay = NULL;
            return;
        }
        lv_obj_set_style_text_font(speed_label, LV_FONT_DEFAULT, LV_PART_MAIN);
        lv_obj_align(speed_label, LV_ALIGN_CENTER, 0, 0);
    }

    lv_obj_move_foreground(speed_overlay);
    lv_label_set_text_fmt(speed_label, "%u", (unsigned int)shown_speed);
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
