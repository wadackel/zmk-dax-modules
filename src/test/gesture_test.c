/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Stands in for a client over Studio RPC, which native_sim tests do not have: shortly after boot
 * it edits the gestures of tests/gesture/runtime-edit through the dax settings layer.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <dax/settings.h>
#include <dt-bindings/dax/gesture.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

extern const struct dax_setting dax_gesture_slot;

static void encode(uint8_t profile, uint32_t pattern, uint32_t keycode, uint8_t value[15]) {
    value[0] = profile;
    sys_put_le32(pattern, &value[1]);
    sys_put_le16(zmk_behavior_get_local_id("key_press"), &value[5]);
    sys_put_le32(keycode, &value[7]);
    sys_put_le32(0, &value[11]);
}

static int edit(void) {
    uint8_t value[15];

    /* A gesture in a profile the processor does not have is refused. */
    encode(2, DAX_GESTURE_RIGHT, D, value);
    if (dax_setting_check(&dax_gesture_slot, 0, value, sizeof(value)) != -EINVAL) {
        LOG_ERR("gesture test: a gesture in profile 2 was accepted");
        return -EINVAL;
    }

    /* Slot 0 (always, right: A) now taps D; slot 2 (held, down-left: C) is removed. */
    encode(0, DAX_GESTURE_RIGHT, D, value);
    int ret = dax_setting_set(&dax_gesture_slot, 0, value, sizeof(value));
    const uint8_t deleted = 0xFE;
    return ret ?: dax_setting_set(&dax_gesture_slot, 2, &deleted, 1);
}

static void run_edit(struct k_work *work) {
    ARG_UNUSED(work);
    if (edit() == 0) {
        LOG_INF("PASS: dax_gesture_test_edit");
    }
}

static K_WORK_DELAYABLE_DEFINE(edit_work, run_edit);

/* Scheduled rather than run here, so the writes happen outside the load that raised this event. */
static int on_loaded(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    k_work_schedule(&edit_work, K_MSEC(1));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_gesture_test, on_loaded);
ZMK_SUBSCRIPTION(dax_gesture_test, dax_settings_loaded);
