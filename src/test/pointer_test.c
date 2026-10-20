/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Stands in for a client over Studio RPC, which native_sim tests do not have. Each scenario
 * matches one directory under tests/pointer/, whose keymap times its reports around these writes.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <dax/pointer.h>
#include <dax/settings.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static void log_result(const char *what, int ret) { LOG_INF("pointer test: %s: %d", what, ret); }

#if !IS_ENABLED(CONFIG_ZMK_DAX_POINTER_TEST_REORDER)
static const struct dax_setting *find(const char *key) {
    STRUCT_SECTION_FOREACH(dax_setting, setting) {
        if (strcmp(setting->owner, DAX_POINTER_SUBSYSTEM_ID) == 0 &&
            strcmp(setting->key, key) == 0) {
            return setting;
        }
    }
    return NULL;
}

/* As the dax_settings RPC writes: checked against the type, then set in RAM. */
static int write_int32(const char *key, int32_t value) {
    const struct dax_setting *setting = find(key);
    uint8_t bytes[4];
    sys_put_le32(value, bytes);
    int ret = setting ? dax_setting_check(setting, 0, bytes, sizeof(bytes)) : -ENOENT;
    return ret ?: dax_setting_set(setting, 0, bytes, sizeof(bytes));
}

#if IS_ENABLED(CONFIG_ZMK_DAX_POINTER_TEST_RELOAD)
static int write_bool(const char *key, bool value) {
    const struct dax_setting *setting = find(key);
    uint8_t byte = value;
    int ret = setting ? dax_setting_check(setting, 0, &byte, 1) : -ENOENT;
    return ret ?: dax_setting_set(setting, 0, &byte, 1);
}
#endif
#endif

#if IS_ENABLED(CONFIG_ZMK_DAX_POINTER_TEST_EDIT_DISCARD)
/* tests/pointer/edit-discard reports right 2 from 1000 to 1300 ms and again from 1600 to 1900 ms. */
static int setup(void) {
    log_result("write scale_div 40000", write_int32("scale_div", 40000));
    log_result("write rotation 361", write_int32("rotation", 361));
    log_result("write scale_mul 3", write_int32("scale_mul", 3));
    log_result("write rotation 90", write_int32("rotation", 90));
    return 0;
}

static void discard_handler(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t changed = 0;
    int ret = dax_settings_discard(DAX_POINTER_SUBSYSTEM_ID, &changed);
    LOG_INF("pointer test: discarded %u slot(s): %d", changed, ret);
}

static K_WORK_DELAYABLE_DEFINE(discard_work, discard_handler);

static void after_setup(void) { k_work_schedule(&discard_work, K_MSEC(1500)); }
#elif IS_ENABLED(CONFIG_ZMK_DAX_POINTER_TEST_RELOAD)
/* tests/pointer/reload reports X 1 at 1000 ms. The flash file of native_sim outlives a run, so
 * every slot is emptied in flash first. */
static int setup(void) {
    int ret = 0;
    STRUCT_SECTION_FOREACH(dax_setting, setting) {
        if (strcmp(setting->owner, DAX_POINTER_SUBSYSTEM_ID) == 0) {
            for (uint16_t slot = 0; slot < setting->count; slot++) {
                ret = ret ?: dax_setting_unset(setting, slot);
            }
        }
    }
    ret = ret ?: dax_settings_save(DAX_POINTER_SUBSYSTEM_ID, NULL);

    ret = ret ?: write_int32("scale_mul", 4);
    uint32_t written = 0;
    ret = ret ?: dax_settings_save(DAX_POINTER_SUBSYSTEM_ID, &written);
    LOG_INF("pointer test: saved %u slot(s)", written);
    /* Left unsaved, so the reload is seen to happen: it must be gone afterwards. */
    ret = ret ?: write_bool("x_invert", true);
    ret = ret ?: dax_settings_test_reload(DAX_POINTER_SUBSYSTEM_ID);
    return ret;
}

static void after_setup(void) {}
#elif IS_ENABLED(CONFIG_ZMK_DAX_POINTER_TEST_REORDER)
/* tests/pointer/temp-layer holds the layer at position 1. Moving that layer to the end puts the
 * layer with id 2 at position 1, so holding id 1 instead would show in the keys. */
static int setup(void) {
    log_result("move layer 1 to 2", zmk_keymap_move_layer(1, 2));
    LOG_INF("pointer test: position 1 is layer id %u", zmk_keymap_layer_index_to_id(1));
    return 0;
}

static void after_setup(void) {}
#endif

static void run_setup(struct k_work *work) {
    ARG_UNUSED(work);
    if (setup() == 0) {
        LOG_INF("PASS: dax_pointer_test_setup");
    }
    after_setup();
}

static K_WORK_DELAYABLE_DEFINE(setup_work, run_setup);

/* After settings_load(), like a client's writes; scheduled rather than run here so the writes
 * happen outside the load that raised this event. */
static int on_loaded(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    k_work_schedule(&setup_work, K_MSEC(1));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_pointer_test, on_loaded);
ZMK_SUBSCRIPTION(dax_pointer_test, dax_settings_loaded);
