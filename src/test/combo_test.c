/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 *
 * Adapted from the self-test setup of cormoran/zmk-feature-runtime-combo
 * (src/runtime_combo/runtime_combo.c at d9490e7). It stands in for a client over Studio RPC, which
 * native_sim tests do not have. Each scenario matches one directory under tests/combo/.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/zmk/custom_settings.h>
#include <dax/combo.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const char *const key_press = "key_press";

static int write_combo(uint32_t slot, uint32_t keycode, const int32_t *positions, uint8_t len,
                       uint16_t timeout_ms, uint16_t require_prior_idle_ms) {
    struct dax_combo_config combo = {
        .enabled = true,
        .key_position_len = len,
        .behavior = {.behavior_dev = key_press, .param1 = keycode},
        .timeout_ms = timeout_ms,
        .require_prior_idle_ms = require_prior_idle_ms,
    };
    for (uint8_t i = 0; i < len; i++) {
        combo.key_positions[i] = positions[i];
    }
    int ret = dax_combo_write(slot, &combo, false);
    if (ret < 0) {
        LOG_ERR("combo test: writing slot %u failed: %d", slot, ret);
    }
    return ret;
}

#if IS_ENABLED(CONFIG_ZMK_DAX_COMBO_TEST_OVERRIDES)
static int setup(void) {
    int ret = write_combo(0, B, (const int32_t[]){0, 1}, 2, 0, 0);
    ret = ret ?: write_combo(1, C, (const int32_t[]){2, 3}, 2, 0, 0);
    ret = ret ?: write_combo(2, D, (const int32_t[]){2, 3, 4}, 3, 0, 0);
    ret = ret ?: write_combo(3, E, (const int32_t[]){5, 6}, 2, 0, 1000);
    return ret ?: write_combo(4, F, (const int32_t[]){7, 8}, 2, 1000, 0);
}
#elif IS_ENABLED(CONFIG_ZMK_DAX_COMBO_TEST_DEFAULTS)
/* Slot numbers and positions match the dax,runtime-combo-defaults node of tests/combo/defaults:
 * slot 1's default is left untouched, slot 2's is tombstoned, slot 3's is overridden to a
 * different behavior, and slot 4's is tombstoned then restored by a reset. */
static int setup(void) {
    int ret = write_combo(0, B, (const int32_t[]){0, 1}, 2, 0, 0);
    ret = ret ?: dax_combo_delete(2, false);
    ret = ret ?: write_combo(3, J, (const int32_t[]){6, 7}, 2, 0, 0);
    ret = ret ?: dax_combo_delete(4, false);
    return ret ?: dax_combo_reset(4);
}
#elif IS_ENABLED(CONFIG_ZMK_DAX_COMBO_TEST_DISCARD)
/* Both writes are memory only, so an unscoped discard reverts them to empty slots. */
static int setup(void) {
    int ret = write_combo(0, B, (const int32_t[]){0, 1}, 2, 0, 0);
    ret = ret ?: write_combo(1, C, (const int32_t[]){2, 3}, 2, 0, 0);
    return ret ?: zmk_custom_settings_discard_scope(DAX_COMBO_SUBSYSTEM_ID, NULL, NULL, NULL);
}
#elif IS_ENABLED(CONFIG_ZMK_DAX_COMBO_TEST_INVALID)
/* Slot 0 names a key position past the end of the keymap; slot 1 is a usable combo. */
static int setup(void) {
    int ret = write_combo(0, B, (const int32_t[]){0, ZMK_KEYMAP_LEN}, 2, 0, 0);
    return ret ?: write_combo(1, C, (const int32_t[]){1, 2}, 2, 0, 0);
}
#elif IS_ENABLED(CONFIG_ZMK_DAX_COMBO_TEST_PENDING)
/* The edit lands while the default combo of tests/combo/pending-rebuild is held. */
static void edit_held_combo(struct k_work *work) {
    ARG_UNUSED(work);
    int ret = write_combo(0, Y, (const int32_t[]){0, 1}, 2, 0, 0);
    LOG_INF("combo test: edited the held combo: %d", ret);
}

static K_WORK_DELAYABLE_DEFINE(edit_work, edit_held_combo);

static int on_position(const zmk_event_t *eh) {
    static bool scheduled;
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (!scheduled && ev->state && ev->position == 1) {
        scheduled = true;
        k_work_schedule(&edit_work, K_MSEC(10));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_combo_test_position, on_position);
ZMK_SUBSCRIPTION(dax_combo_test_position, zmk_position_state_changed);

static int setup(void) { return 0; }
#endif

static void run_setup(struct k_work *work) {
    ARG_UNUSED(work);
    if (setup() == 0) {
        LOG_INF("PASS: dax_combo_test_setup");
    }
}

static K_WORK_DELAYABLE_DEFINE(setup_work, run_setup);

/* After settings_load(), like a client's writes; scheduled rather than run here so the writes
 * happen outside the load that raised this event. */
static int on_initialized(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    k_work_schedule(&setup_work, K_MSEC(1));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_combo_test, on_initialized);
ZMK_SUBSCRIPTION(dax_combo_test, zmk_custom_settings_initialized);
