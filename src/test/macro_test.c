/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Stands in for a client over Studio RPC, which native_sim tests do not have. Each scenario
 * matches one directory under tests/macro/, whose keymap holds the defaults it names.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <dax/macro.h>
#include <dax/settings.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const char *const sources[] = {
    [DAX_MACRO_SOURCE_EMPTY] = "empty",
    [DAX_MACRO_SOURCE_DEFAULT] = "default",
    [DAX_MACRO_SOURCE_OVERRIDDEN] = "overridden",
    [DAX_MACRO_SOURCE_RUNTIME] = "runtime",
};

/* Every slot that shows a macro, with a letter for each step: Down, Up, Tap, Wait, Keys. */
static void log_slots(const char *step) {
    for (uint32_t slot = 0; slot < CONFIG_ZMK_DAX_MACRO_COUNT; slot++) {
        char name[CONFIG_ZMK_DAX_MACRO_NAME_MAX + 1];
        uint8_t body[CONFIG_ZMK_DAX_MACRO_MAX_BYTES];
        int len = dax_macro_read(slot, name, sizeof(name), body, sizeof(body));
        if (len < 0) {
            continue;
        }
        char kinds[16] = "";
        struct dax_macro_step macro_step;
        size_t offset = 0;
        for (size_t i = 0;
             i < sizeof(kinds) - 1 && dax_macro_next_step(body, len, &offset, &macro_step) == 0;
             i++) {
            kinds[i] = "?DUTWK"[macro_step.kind];
            kinds[i + 1] = '\0';
        }
        LOG_INF("macro test: %s: slot %u %s, %d bytes, steps %s, %s, unsaved %d", step, slot, name,
                len, kinds, sources[dax_macro_source(slot)], dax_macro_is_unsaved(slot));
    }
}

static void log_result(const char *what, int ret) { LOG_INF("macro test: %s: %d", what, ret); }

static struct dax_macro_step tap_key(uint32_t keycode) {
    return (struct dax_macro_step){
        .kind = DAX_MACRO_STEP_TAP,
        .behavior_id = zmk_behavior_get_local_id(DEVICE_DT_NAME(DT_NODELABEL(kp))),
        .param1 = keycode,
    };
}

#if IS_ENABLED(CONFIG_ZMK_DAX_MACRO_TEST_DEFAULTS)
static int setup(void) {
    log_slots("at boot");
    return 0;
}
#elif IS_ENABLED(CONFIG_ZMK_DAX_MACRO_TEST_EDIT_DISCARD)
/* tests/macro/edit-discard has four slots, with defaults in 0 (esc_lang2) and 2 (other). */
static int setup(void) {
    uint32_t slot = UINT32_MAX;
    struct dax_macro_step step = tap_key(B);
    struct dax_macro_step unknown = {.kind = DAX_MACRO_STEP_TAP, .behavior_id = 0xFFFE};
    struct dax_macro_step keys = {
        .kind = DAX_MACRO_STEP_KEY_TAP_SEQUENCE, .key_count = 2, .keys = {0x07, 0x08}};
    struct dax_macro_step delay = {.kind = DAX_MACRO_STEP_DELAY, .delay_ms = 5};

    log_slots("at boot");
    log_result("delete the default in 2", dax_macro_delete(2));
    log_result("create one", dax_macro_create("one", &slot));
    LOG_INF("macro test: one is in slot %u", slot);
    log_result("create two", dax_macro_create("two", &slot));
    LOG_INF("macro test: two is in slot %u", slot);
    log_result("create a third", dax_macro_create("three", NULL));
    log_result("create a duplicate", dax_macro_create("esc_lang2", NULL));
    log_result("rename 1 to a duplicate", dax_macro_rename(1, "esc_lang2"));
    log_result("rename 1", dax_macro_rename(1, "uno"));
    log_result("append a tap to 1", dax_macro_append_step(1, &step));
    log_result("append keys to 1", dax_macro_append_step(1, &keys));
    log_result("append an unknown behavior to 1", dax_macro_append_step(1, &unknown));
    log_result("set step 5 of 1", dax_macro_set_step(1, 5, &delay));
    log_result("set the step count of 1 to 3", dax_macro_set_step_count(1, 3));
    log_result("set step 0 of 0", dax_macro_set_step(0, 0, &delay));
    log_result("rename 0", dax_macro_rename(0, "renamed"));
    log_result("set the step count of 3 to 0", dax_macro_set_step_count(3, 0));
    log_result("delete 3", dax_macro_delete(3));
    log_result("append to the deleted 3", dax_macro_append_step(3, &step));
    log_slots("edited");

    uint32_t changed = 0;
    int ret = dax_macro_discard(&changed);
    LOG_INF("macro test: discarded %u slot(s)", changed);
    log_slots("discarded");
    return ret;
}
#elif IS_ENABLED(CONFIG_ZMK_DAX_MACRO_TEST_RELOAD)
/* tests/macro/reload has four slots, with defaults in 0 (esc_lang2) and 2 (other). The flash file
 * of native_sim outlives a run, so the slots are emptied in flash first. */
static int setup(void) {
    int ret = 0;
    for (uint32_t slot = 0; slot < CONFIG_ZMK_DAX_MACRO_COUNT; slot++) {
        ret = ret ?: dax_macro_reset(slot);
    }
    ret = ret ?: dax_macro_write_tap_ms(CONFIG_ZMK_DAX_MACRO_DEFAULT_TAP_MS);
    ret = ret ?: dax_macro_save(NULL);
    ret = ret ?: dax_settings_test_reload(DAX_MACRO_SUBSYSTEM_ID);
    log_slots("emptied");

    uint32_t slot = UINT32_MAX;
    struct dax_macro_step step = tap_key(B);
    struct dax_macro_step keys = {
        .kind = DAX_MACRO_STEP_KEY_TAP_SEQUENCE, .key_count = 1, .keys = {0x07}};
    ret = ret ?: dax_macro_create("saved", &slot);
    ret = ret ?: dax_macro_append_step(slot, &step);
    ret = ret ?: dax_macro_append_step(slot, &keys);
    ret = ret ?: dax_macro_create("gone", NULL);
    ret = ret ?: dax_macro_rename(0, "esc");
    ret = ret ?: dax_macro_write_tap_ms(40);
    uint32_t written = 0;
    ret = ret ?: dax_macro_save(&written);
    LOG_INF("macro test: saved %u record(s)", written);

    log_result("delete the default in 2", dax_macro_delete(2));
    ret = ret ?: dax_macro_delete(3);
    ret = ret ?: dax_macro_save(&written);
    LOG_INF("macro test: saved %u record(s)", written);

    /* What another firmware could have left: a record whose name runs past its end. */
    static const uint8_t unreadable[] = {5, 'b', 'a', 'd'};
    ret = ret ?: settings_save_one("dax/" DAX_MACRO_SUBSYSTEM_ID "/macro/3", unreadable,
                                   sizeof(unreadable));

    ret = ret ?: dax_settings_test_reload(DAX_MACRO_SUBSYSTEM_ID);
    log_slots("reloaded");
    LOG_INF("macro test: tap_ms %u", dax_macro_tap_ms());
    log_result("create new", dax_macro_create("new", &slot));
    LOG_INF("macro test: new is in slot %u", slot);
    return ret ?: dax_macro_discard(NULL);
}
#endif

static void run_setup(struct k_work *work) {
    ARG_UNUSED(work);
    if (setup() == 0) {
        LOG_INF("PASS: dax_macro_test_setup");
    }
}

static K_WORK_DELAYABLE_DEFINE(setup_work, run_setup);

/* After settings_load(), like a client's writes; scheduled rather than run here so the writes
 * happen outside the load that raised this event. */
static int on_loaded(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    k_work_schedule(&setup_work, K_MSEC(1));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_macro_test, on_loaded);
ZMK_SUBSCRIPTION(dax_macro_test, dax_settings_loaded);
