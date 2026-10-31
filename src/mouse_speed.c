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

static void mouse_speed_display_refresh(void);

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
    mouse_speed_display_refresh();
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

#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)

#include <zmk/display/status_screen.h>
#include <zmk/display/widgets/battery_status.h>
#include <zmk/display/widgets/layer_status.h>
#include <zmk/display/widgets/output_status.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>

LV_FONT_DECLARE(lv_font_montserrat_16)
LV_FONT_DECLARE(lv_font_montserrat_12)

/* Private-use character. Montserrat has no mouse glyph. */
#define MOUSE_ICON "\xEE\x80\x80"

/* 12x9, 4bpp, high nibble first. Same glyph path as the status icons.
 *   ..########..
 *   .##########.
 *   .###....###.
 *   .###....###.
 *   .##########.
 *   .####..####.
 *   .##########.
 *   ..########..
 *   ...######...
 */
static const uint8_t mouse_icon_bitmap[] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x0F, 0xFF, 0x00, 0x00,
    0xFF, 0xF0, 0x0F, 0xFF, 0x00, 0x00, 0xFF, 0xF0, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x0F, 0xFF,
    0xF0, 0x0F, 0xFF, 0xF0, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    0x00, 0x0F, 0xFF, 0xFF, 0xF0, 0x00,
};

static const lv_font_fmt_txt_glyph_dsc_t mouse_icon_glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 192, .box_w = 12, .box_h = 9, .ofs_x = 0, .ofs_y = 0},
};

static const lv_font_fmt_txt_cmap_t mouse_icon_cmaps[] = {
    {
        .range_start = 0xE000,
        .range_length = 1,
        .glyph_id_start = 1,
        .unicode_list = NULL,
        .glyph_id_ofs_list = NULL,
        .list_length = 0,
        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY,
    },
};

static const lv_font_fmt_txt_dsc_t mouse_icon_dsc = {
    .glyph_bitmap = mouse_icon_bitmap,
    .glyph_dsc = mouse_icon_glyph_dsc,
    .cmaps = mouse_icon_cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,
};

/* line_height and base_line match Montserrat 12. Digits and the space come
 * from that font; only the private-use character is drawn from this one.
 * An ofs_y of 0 puts the 9px icon on the same band as the 12px digits. */
static const lv_font_t mouse_icon_font = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 15,
    .base_line = 3,
    .subpx = LV_FONT_SUBPX_NONE,
    .kerning = LV_FONT_KERNING_NONE,
    .underline_position = 0,
    .underline_thickness = 0,
    .dsc = &mouse_icon_dsc,
    .fallback = &lv_font_montserrat_12,
};

static lv_obj_t *speed_label;

static void speed_label_update(uint16_t speed) {
    if (speed_label == NULL) {
        return;
    }
    lv_label_set_text_fmt(speed_label, "%u " MOUSE_ICON, (unsigned int)speed);
}

static uint16_t speed_label_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    return mouse_speed_current();
}

ZMK_DISPLAY_WIDGET_LISTENER(mouse_speed_status, uint16_t, speed_label_update, speed_label_get_state)
ZMK_SUBSCRIPTION(mouse_speed_status, zmk_endpoint_changed);
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(mouse_speed_status, zmk_ble_active_profile_changed);
#endif

static void mouse_speed_display_refresh(void) { mouse_speed_status_cb(NULL); }

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS)
    static struct zmk_widget_battery_status battery_status_widget;

    zmk_widget_battery_status_init(&battery_status_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_battery_status_obj(&battery_status_widget),
                               &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(zmk_widget_battery_status_obj(&battery_status_widget), LV_ALIGN_TOP_RIGHT, 0, 0);
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
    static struct zmk_widget_output_status output_status_widget;

    zmk_widget_output_status_init(&output_status_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_output_status_obj(&output_status_widget),
                               &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(zmk_widget_output_status_obj(&output_status_widget), LV_ALIGN_TOP_LEFT, 0, 0);
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
    static struct zmk_widget_layer_status layer_status_widget;

    zmk_widget_layer_status_init(&layer_status_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_layer_status_obj(&layer_status_widget),
                               &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_status_widget), LV_ALIGN_BOTTOM_LEFT, 0, 0);
#endif

    /* Right edge stays put when the number grows from 90 to 3600. */
    speed_label = lv_label_create(screen);
    lv_obj_set_style_text_font(speed_label, &mouse_icon_font, LV_PART_MAIN);
    lv_obj_align(speed_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    mouse_speed_status_init();

    return screen;
}

#else

static void mouse_speed_display_refresh(void) {}

#endif /* custom status screen */

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
    mouse_speed_display_refresh();
    return speed;
}

#endif /* peripheral */
