/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Stands in for a client over Studio RPC, which native_sim tests do not have. Each scenario
 * matches one directory under tests/encoder/, whose encoder mock turns around these writes.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <dax/encoder.h>
#include <dax/settings.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct dax_setting *binding_setting(void) {
    STRUCT_SECTION_FOREACH(dax_setting, setting) {
        if (strcmp(setting->owner, DAX_ENCODER_SUBSYSTEM_ID) == 0 &&
            strcmp(setting->key, "binding") == 0) {
            return setting;
        }
    }
    return NULL;
}

/* As the dax_settings RPC writes: checked against the type, then set in RAM. */
static int write_binding(uint16_t slot, const char *behavior, uint32_t param1, uint16_t tap_ms) {
    const struct dax_setting *setting = binding_setting();
    uint8_t record[DAX_ENCODER_RECORD_SIZE];
    sys_put_le16(zmk_behavior_get_local_id(behavior), &record[0]);
    sys_put_le32(param1, &record[2]);
    sys_put_le32(0, &record[6]);
    sys_put_le16(tap_ms, &record[10]);
    int ret = dax_setting_check(setting, slot, record, sizeof(record));
    return ret ?: dax_setting_set(setting, slot, record, sizeof(record));
}

static const char *const key_press = DEVICE_DT_NAME(DT_NODELABEL(kp));

#if IS_ENABLED(CONFIG_ZMK_DAX_ENCODER_TEST_DEFAULTS)
static void log_default(const char *what, uint16_t slot) {
    uint8_t record[DAX_ENCODER_RECORD_SIZE];
    int len = dax_setting_read(binding_setting(), slot, record, sizeof(record), NULL);
    if (len != DAX_ENCODER_RECORD_SIZE) {
        LOG_INF("encoder test: %s: none (%d)", what, len);
        return;
    }
    const char *name = zmk_behavior_find_behavior_name_from_local_id(sys_get_le16(&record[0]));
    LOG_INF("encoder test: %s: %s 0x%x tap %u", what, name ? name : "?", sys_get_le32(&record[2]),
            sys_get_le16(&record[10]));
}

/* tests/encoder/defaults: layer 0 is dax_enc_ab, layer 1 &inc_dec_kp, layer 2 dax_enc_cd. */
static int setup(void) {
    log_default("layer 0 cw", DAX_ENCODER_SLOT(0, 0, DAX_ENCODER_CW));
    log_default("layer 0 ccw", DAX_ENCODER_SLOT(0, 0, DAX_ENCODER_CCW));
    log_default("layer 1 cw", DAX_ENCODER_SLOT(0, 1, DAX_ENCODER_CW));
    log_default("layer 2 cw", DAX_ENCODER_SLOT(0, 2, DAX_ENCODER_CW));
    log_default("layer 2 ccw", DAX_ENCODER_SLOT(0, 2, DAX_ENCODER_CCW));
    LOG_INF("encoder test: write to layer 1: %d",
            write_binding(DAX_ENCODER_SLOT(0, 1, DAX_ENCODER_CW), key_press, X, 20));
    LOG_INF("encoder test: write an encoder behavior: %d",
            write_binding(DAX_ENCODER_SLOT(0, 0, DAX_ENCODER_CW),
                          DEVICE_DT_NAME(DT_NODELABEL(dax_enc_cd)), 0, 20));
    LOG_INF("encoder test: write a 1001 ms tap: %d",
            write_binding(DAX_ENCODER_SLOT(0, 0, DAX_ENCODER_CW), key_press, X, 1001));
    return 0;
}

static void after_setup(void) {}
#elif IS_ENABLED(CONFIG_ZMK_DAX_ENCODER_TEST_EDIT_DISCARD)
static void discard_handler(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t changed = 0;
    int ret = dax_settings_discard(DAX_ENCODER_SUBSYSTEM_ID, &changed);
    LOG_INF("encoder test: discarded %u slot(s): %d", changed, ret);
}

static K_WORK_DELAYABLE_DEFINE(discard_work, discard_handler);

/* tests/encoder/edit-discard turns clockwise at 1000 ms and again at 2000 ms. */
static int setup(void) {
    LOG_INF("encoder test: write E: %d",
            write_binding(DAX_ENCODER_SLOT(0, 0, DAX_ENCODER_CW), key_press, E, 20));
    return 0;
}

/* At a time since boot, between the two turns: settings load a while after boot. */
static void after_setup(void) { k_work_schedule(&discard_work, K_TIMEOUT_ABS_MS(1500)); }
#elif IS_ENABLED(CONFIG_ZMK_DAX_ENCODER_TEST_RELOAD)
/* tests/encoder/reload turns clockwise at 1000 ms and counterclockwise at 1200 ms. The flash file
 * of native_sim outlives a run, so every slot is emptied in flash first. */
static int setup(void) {
    const struct dax_setting *setting = binding_setting();
    int ret = 0;
    for (uint16_t slot = 0; slot < setting->count; slot++) {
        ret = ret ?: dax_setting_unset(setting, slot);
    }
    ret = ret ?: dax_settings_save(DAX_ENCODER_SUBSYSTEM_ID, NULL);

    ret = ret ?: write_binding(DAX_ENCODER_SLOT(0, 0, DAX_ENCODER_CW), key_press, E, 20);
    uint32_t written = 0;
    ret = ret ?: dax_settings_save(DAX_ENCODER_SUBSYSTEM_ID, &written);
    LOG_INF("encoder test: saved %u slot(s)", written);
    /* Left unsaved, so the reload is seen to happen: it must be gone afterwards. */
    ret = ret ?: write_binding(DAX_ENCODER_SLOT(0, 0, DAX_ENCODER_CCW), key_press, F, 20);
    return ret ?: dax_settings_test_reload(DAX_ENCODER_SUBSYSTEM_ID);
}

static void after_setup(void) {}
#endif

static void run_setup(struct k_work *work) {
    ARG_UNUSED(work);
    if (setup() == 0) {
        LOG_INF("PASS: dax_encoder_test_setup");
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

ZMK_LISTENER(dax_encoder_test, on_loaded);
ZMK_SUBSCRIPTION(dax_encoder_test, dax_settings_loaded);
