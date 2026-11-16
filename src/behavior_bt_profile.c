/*
 * Copyright (c) 2026 Greg Grunschel
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_bt_profile

#include <errno.h>
#include <stdio.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
#include <lvgl.h>
#include <zmk/display.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <output_status.h>
#endif

/* ZMK remembers USB as the preferred output, including after the cable is
 * gone and the active profile has dropped. Selecting the profile that is
 * already active is also a no-op, so it does not start advertising again.
 * Prefer Bluetooth whenever USB is not enumerated. A fresh advertisement is
 * required when the link dropped out of range: ZMK will not restart one it
 * already considers running. The bytes match the open advertisement in ZMK
 * ble.c, which is not aimed at one profile. */
extern int update_advertising(void);

static struct bt_data open_ad[] = {
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, BT_BYTES_LIST_LE16(CONFIG_BT_DEVICE_APPEARANCE)),
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
                  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static bool usb_hid_ready;
K_MUTEX_DEFINE(bt_profile_lock);

static void prefer_output_for_cable(void) {
    enum zmk_transport transport =
        usb_hid_ready ? ZMK_TRANSPORT_USB : ZMK_TRANSPORT_BLE;
    zmk_endpoint_set_preferred_transport(transport);
}

#if IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
static lv_obj_t *sofle_output_label;

struct sofle_output_state {
    char text[20];
};

static struct sofle_output_state sofle_output_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    struct sofle_output_state state = {0};
    struct zmk_endpoint_instance selected = zmk_endpoint_get_selected();
    enum zmk_transport transport = selected.transport;
    bool live = transport != ZMK_TRANSPORT_NONE;

    if (!live) {
        transport = zmk_endpoint_get_preferred_transport();
    }

    if (transport == ZMK_TRANSPORT_USB) {
        if (live) {
            snprintf(state.text, sizeof(state.text), "%s", LV_SYMBOL_USB);
        } else {
            snprintf(state.text, sizeof(state.text), "%s %s", LV_SYMBOL_USB, LV_SYMBOL_CLOSE);
        }
        return state;
    }

    if (transport != ZMK_TRANSPORT_BLE) {
        snprintf(state.text, sizeof(state.text), "%s", LV_SYMBOL_CLOSE);
        return state;
    }

    /* A disconnected endpoint is the none transport, whose profile index is
     * always 0. Use the active profile so slots other than the first one
     * do not draw as profile 1 while they reconnect. */
    int number = zmk_ble_active_profile_index() + 1;
    if (selected.transport == ZMK_TRANSPORT_BLE) {
        number = selected.ble.profile_index + 1;
    }

    const char *mark = LV_SYMBOL_SETTINGS;
    if (!zmk_ble_active_profile_is_open()) {
        mark = zmk_ble_active_profile_is_connected() ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE;
    }
    snprintf(state.text, sizeof(state.text), "%s %d %s", LV_SYMBOL_WIFI, number, mark);
    return state;
}

static void sofle_output_update(struct sofle_output_state state) {
    if (sofle_output_label == NULL) {
        return;
    }
    lv_label_set_text(sofle_output_label, state.text);
}

ZMK_DISPLAY_WIDGET_LISTENER(sofle_output, struct sofle_output_state, sofle_output_update,
                            sofle_output_get_state)
ZMK_SUBSCRIPTION(sofle_output, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(sofle_output, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(sofle_output, zmk_usb_conn_state_changed);

int sofle_output_status_init(lv_obj_t *parent) {
    sofle_output_label = lv_label_create(parent);
    sofle_output_init();
    return 0;
}

lv_obj_t *sofle_output_status_obj(void) { return sofle_output_label; }

static void refresh_output_icon(void) {
    if (zmk_display_is_initialized()) {
        sofle_output_cb(NULL);
    }
}
#else
static void refresh_output_icon(void) {}
#endif

static void restart_active_advertising(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(&bt_profile_lock, K_FOREVER);
    bool skip = usb_hid_ready || zmk_ble_active_profile_is_connected();
    k_mutex_unlock(&bt_profile_lock);
    if (skip) {
        return;
    }

    /* Pull ZMK's advertising state forward when it had never started. This is
     * a no-op when ZMK already believes an advertisement is running. */
    int err = update_advertising();
    if (err) {
        LOG_WRN("Advertising update failed (%d)", err);
    }
    if (zmk_usb_is_hid_ready() || zmk_ble_active_profile_is_connected()) {
        return;
    }

    err = bt_le_adv_stop();
    if (err && err != -EALREADY) {
        LOG_WRN("Could not stop advertising (%d)", err);
    }

    err = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_NAME |
                                              BT_LE_ADV_OPT_FORCE_NAME_IN_AD,
                                          BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2,
                                          NULL),
                          open_ad, ARRAY_SIZE(open_ad), NULL, 0);
    if (err && err != -EALREADY) {
        LOG_ERR("Could not restart advertising (%d)", err);
        return;
    }
    LOG_INF("Restarted advertising for profile %d", zmk_ble_active_profile_index());
    refresh_output_icon();
}

K_WORK_DEFINE(restart_adv_work, restart_active_advertising);

static void boot_prefer_output(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&bt_profile_lock, K_FOREVER);
    usb_hid_ready = zmk_usb_is_hid_ready();
    prefer_output_for_cable();
    k_mutex_unlock(&bt_profile_lock);
    refresh_output_icon();
}

K_WORK_DELAYABLE_DEFINE(boot_pref_work, boot_prefer_output);

static int usb_output_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    bool ready = zmk_usb_is_hid_ready();
    bool restart = false;

    k_mutex_lock(&bt_profile_lock, K_FOREVER);
    if (ready != usb_hid_ready) {
        usb_hid_ready = ready;
        prefer_output_for_cable();
        restart = !ready && !zmk_ble_active_profile_is_connected();
    }
    k_mutex_unlock(&bt_profile_lock);

    if (restart) {
        k_work_submit(&restart_adv_work);
    }
    refresh_output_icon();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(bt_profile_usb, usb_output_listener);
ZMK_SUBSCRIPTION(bt_profile_usb, zmk_usb_conn_state_changed);

static int bt_profile_init(void) {
    usb_hid_ready = zmk_usb_is_hid_ready();
    /* Settings load can restore a saved USB preference after this init. */
    k_work_schedule(&boot_pref_work, K_MSEC(1500));
    return 0;
}

SYS_INIT(bt_profile_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int on_bt_profile_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);
    uint32_t index = binding->param1;

    if (index >= ZMK_BLE_PROFILE_COUNT) {
        LOG_ERR("Bluetooth profile %u is outside 0..%d", index, ZMK_BLE_PROFILE_COUNT - 1);
        return -EINVAL;
    }

    bool restart = false;
    int err = 0;

    k_mutex_lock(&bt_profile_lock, K_FOREVER);
    if (!usb_hid_ready) {
        zmk_endpoint_set_preferred_transport(ZMK_TRANSPORT_BLE);
    }

    if ((uint8_t)index != zmk_ble_active_profile_index()) {
        err = zmk_ble_prof_select(index);
    } else if (!usb_hid_ready && !zmk_ble_active_profile_is_connected()) {
        restart = true;
    }
    k_mutex_unlock(&bt_profile_lock);

    if (restart) {
        k_work_submit(&restart_adv_work);
    }
    refresh_output_icon();
    return err == 0 ? ZMK_BEHAVIOR_OPAQUE : err;
}

static int on_bt_profile_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api bt_profile_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_bt_profile_pressed,
    .binding_released = on_bt_profile_released,
};

#define BT_PROFILE_INST(n)                                                                         \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &bt_profile_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BT_PROFILE_INST)
