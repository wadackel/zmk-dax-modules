/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Gesture tuning, profiles and gestures as custom settings under the `dax_gesture` subsystem, so a
 * settings app can change them without a rebuild. The devicetree values are the defaults.
 *
 *   stroke_size, idle_ms, cooldown_ms, partial_ms   int32
 *   eager                                           bool
 *   p<i>/name                                       string, the profile's node name
 *   p<i>/layers, p<i>/require                       int32 bitmask of layer positions
 *   p<i>/suppress                                   bool
 *   g<j>                                            bytes, one gesture slot (see below)
 */

#define DT_DRV_COMPAT dax_input_processor_gesture

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <cormoran/zmk/custom_settings.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>

#include "gesture_model.h"

LOG_MODULE_DECLARE(dax_gesture, CONFIG_ZMK_LOG_LEVEL);

#define SUBSYSTEM_ID "dax_gesture"
#define GESTURE_NODE DT_DRV_INST(0)
#define SLOT_COUNT CONFIG_ZMK_DAX_GESTURE_MAX_GESTURES

/*
 * A gesture slot on the wire, little-endian:
 *   [0]      profile index; SLOT_UNSET or SLOT_DELETED in a one-byte value
 *   [1..4]   pattern, four bits per stroke
 *   [5..6]   behavior local id
 *   [7..10]  param1
 *   [11..14] param2
 * A slot that was never written is SLOT_UNSET and follows the devicetree; it is filled in with
 * that default in memory after boot so a client can read it. SLOT_DELETED is what a client
 * writes to remove a gesture, devicetree default included.
 */
#define SLOT_UNSET 0xFF
#define SLOT_DELETED 0xFE
#define SLOT_SIZE 15

/* The subsystem these settings belong to is registered in gesture_rpc.c when Studio RPC is on;
 * a client needs that registration to get an index to address them by. */

#define NUMBER_SETTING(name, key, initial, min, max)                                               \
    ZMK_CUSTOM_SETTING_DEFINE(name, SUBSYSTEM_ID, key, ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,        \
                              ZMK_CUSTOM_SETTING_VALUE_INT32(initial),                             \
                              ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,                       \
                              ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                              \
                              ZMK_CUSTOM_SETTING_PERMISSION_SECURE,                                \
                              ZMK_CUSTOM_SETTING_RANGE_INT32(min, max))
#define FLAG_SETTING(name, key, initial)                                                           \
    ZMK_CUSTOM_SETTING_DEFINE(name, SUBSYSTEM_ID, key, ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL,         \
                              ZMK_CUSTOM_SETTING_VALUE_BOOL(initial),                              \
                              ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,                       \
                              ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                              \
                              ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT)

NUMBER_SETTING(dax_gesture_stroke_size, "stroke_size", DT_INST_PROP(0, stroke_size), 10, 10000);
NUMBER_SETTING(dax_gesture_idle_ms, "idle_ms", DT_INST_PROP(0, idle_timeout_ms), 0, 10000);
NUMBER_SETTING(dax_gesture_cooldown_ms, "cooldown_ms", DT_INST_PROP(0, gesture_cooldown_ms), 0,
               10000);
NUMBER_SETTING(dax_gesture_partial_ms, "partial_ms", DT_INST_PROP(0, partial_gesture_timeout_ms),
               0, 10000);
FLAG_SETTING(dax_gesture_eager, "eager", DT_INST_PROP(0, eager));

#define LAYER_BIT(node, prop, idx) BIT(DT_PROP_BY_IDX(node, prop, idx))
#define LAYER_MASK(node, prop)                                                                     \
    COND_CODE_1(DT_NODE_HAS_PROP(node, prop), (DT_FOREACH_PROP_ELEM_SEP(node, prop, LAYER_BIT, (|))), (0))

/* Keys are numbered by child index, which is the profile's index in the model as long as no
 * profile node is disabled. */
#define PROFILE_KEY(profile, leaf) "p" STRINGIFY(DT_NODE_CHILD_IDX(profile)) "/" leaf
#define PROFILE_SETTINGS(profile)                                                                  \
    ZMK_CUSTOM_SETTING_DEFINE(_CONCAT(dax_gesture_name_, profile), SUBSYSTEM_ID,                   \
                              PROFILE_KEY(profile, "name"), ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,  \
                              ZMK_CUSTOM_SETTING_VALUE_STRING(DT_NODE_FULL_NAME(profile)),         \
                              ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,                       \
                              ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                              \
                              ZMK_CUSTOM_SETTING_PERMISSION_SECURE,                                \
                              ZMK_CUSTOM_SETTING_NO_CONSTRAINT);                                   \
    NUMBER_SETTING(_CONCAT(dax_gesture_layers_, profile), PROFILE_KEY(profile, "layers"),          \
                   LAYER_MASK(profile, layers), 0, INT32_MAX);                                     \
    NUMBER_SETTING(_CONCAT(dax_gesture_require_, profile), PROFILE_KEY(profile, "require"),        \
                   LAYER_MASK(profile, require_layers), 0, INT32_MAX);                             \
    FLAG_SETTING(_CONCAT(dax_gesture_suppress_, profile), PROFILE_KEY(profile, "suppress"),        \
                 DT_PROP(profile, suppress_movement));

DT_FOREACH_CHILD_STATUS_OKAY(GESTURE_NODE, PROFILE_SETTINGS)
BUILD_ASSERT(DT_CHILD_NUM(GESTURE_NODE) == DT_CHILD_NUM_STATUS_OKAY(GESTURE_NODE),
             "Remove a gesture profile instead of disabling it: setting keys follow child order");

#define SLOT_SETTING(index, _)                                                                     \
    ZMK_CUSTOM_SETTING_DEFINE(dax_gesture_slot_##index, SUBSYSTEM_ID, "g" STRINGIFY(index),        \
                              ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,                                 \
                              ZMK_CUSTOM_SETTING_VALUE_BYTES(SLOT_UNSET),                          \
                              ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,                       \
                              ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,                              \
                              ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT)

LISTIFY(SLOT_COUNT, SLOT_SETTING, (;));

static struct dax_gesture_model *model;
static struct k_mutex *model_lock;
static void (*model_changed)(void);
static struct dax_gesture_slot devicetree_slots[SLOT_COUNT];
static bool seeding;

static bool read_setting(const char *key, struct zmk_custom_setting_value *value) {
    int ret = zmk_custom_setting_read_by_key(SUBSYSTEM_ID, key, value);

    if (ret < 0) {
        LOG_WRN("Failed to read %s: %d", key, ret);
    }
    return ret >= 0;
}

static uint32_t read_number(const char *key, uint32_t fallback) {
    struct zmk_custom_setting_value value;

    return read_setting(key, &value) ? (uint32_t)value.int32_value : fallback;
}

static bool read_flag(const char *key, bool fallback) {
    struct zmk_custom_setting_value value;

    return read_setting(key, &value) ? value.bool_value : fallback;
}

static void encode_slot(const struct dax_gesture_slot *slot, struct zmk_custom_setting_value *value) {
    *value = (struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                               .size = SLOT_SIZE};
    value->bytes_value[0] = slot->profile;
    sys_put_le32(slot->pattern, &value->bytes_value[1]);
    sys_put_le16(zmk_behavior_get_local_id(slot->binding.behavior_dev), &value->bytes_value[5]);
    sys_put_le32(slot->binding.param1, &value->bytes_value[7]);
    sys_put_le32(slot->binding.param2, &value->bytes_value[11]);
}

static struct dax_gesture_slot decode_slot(const struct zmk_custom_setting_value *value,
                                           const struct dax_gesture_slot *fallback) {
    if (value->size == SLOT_SIZE) {
        return (struct dax_gesture_slot){
            .profile = value->bytes_value[0],
            .pattern = sys_get_le32(&value->bytes_value[1]),
            .binding = {.behavior_dev = zmk_behavior_find_behavior_name_from_local_id(
                            sys_get_le16(&value->bytes_value[5])),
                        .param1 = sys_get_le32(&value->bytes_value[7]),
                        .param2 = sys_get_le32(&value->bytes_value[11])},
        };
    }
    if (value->size >= 1 && value->bytes_value[0] == SLOT_UNSET) {
        return *fallback;
    }
    return (struct dax_gesture_slot){.profile = DAX_GESTURE_NO_PROFILE};
}

/* Writes the devicetree default into every unset slot that has one, in memory only. */
static void seed_unset_slots(void) {
    char key[8];
    struct zmk_custom_setting_value value;

    for (size_t i = 0; i < SLOT_COUNT; i++) {
        if (devicetree_slots[i].profile == DAX_GESTURE_NO_PROFILE) {
            continue;
        }
        snprintf(key, sizeof(key), "g%u", (unsigned int)i);
        if (!read_setting(key, &value) || value.size != 1 || value.bytes_value[0] != SLOT_UNSET) {
            continue;
        }
        encode_slot(&devicetree_slots[i], &value);
        int ret = zmk_custom_setting_write_by_key(SUBSYSTEM_ID, key, &value,
                                                  ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        if (ret < 0) {
            LOG_WRN("Failed to seed %s: %d", key, ret);
        }
    }
}

static void load_model(void) {
    char key[16];
    struct zmk_custom_setting_value value;

    model->tuning.stroke_size = read_number("stroke_size", model->tuning.stroke_size);
    model->tuning.idle_timeout_ms = read_number("idle_ms", model->tuning.idle_timeout_ms);
    model->tuning.cooldown_ms = read_number("cooldown_ms", model->tuning.cooldown_ms);
    model->tuning.partial_timeout_ms = read_number("partial_ms", model->tuning.partial_timeout_ms);
    model->tuning.eager = read_flag("eager", model->tuning.eager);

    for (size_t i = 0; i < model->profiles_len; i++) {
        struct dax_gesture_profile *profile = &model->profiles[i];

        snprintf(key, sizeof(key), "p%u/layers", (unsigned int)i);
        profile->layers = read_number(key, profile->layers);
        snprintf(key, sizeof(key), "p%u/require", (unsigned int)i);
        profile->require_layers = read_number(key, profile->require_layers);
        snprintf(key, sizeof(key), "p%u/suppress", (unsigned int)i);
        profile->suppress_movement = read_flag(key, profile->suppress_movement);
    }

    for (size_t i = 0; i < SLOT_COUNT; i++) {
        snprintf(key, sizeof(key), "g%u", (unsigned int)i);
        model->slots[i] = read_setting(key, &value) ? decode_slot(&value, &devicetree_slots[i])
                                                    : devicetree_slots[i];
        if (model->slots[i].profile >= model->profiles_len) {
            model->slots[i].profile = DAX_GESTURE_NO_PROFILE;
        }
    }
    dax_gesture_model_rebuild_views(model);

    size_t used = 0;
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        used += model->slots[i].profile != DAX_GESTURE_NO_PROFILE;
    }
    LOG_DBG("Loaded %u gestures in %u profiles from settings", (unsigned int)used,
            (unsigned int)model->profiles_len);
}

static void refresh_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    /* A discard or reset can leave a default slot unset again; put the default back first.
     * Seeding must stay silent: each write would otherwise send a Studio notification from this
     * work queue, and with no host reading the port the transmit buffer fills after a few of them
     * and the send never returns, which stops the keyboard at boot. */
    seeding = true;
    zmk_custom_settings_notify_suppress_begin();
    seed_unset_slots();
    zmk_custom_settings_notify_suppress_end();
    seeding = false;

    k_mutex_lock(model_lock, K_FOREVER);
    load_model();
    model_changed();
    k_mutex_unlock(model_lock);
}

static K_WORK_DEFINE(refresh_work, refresh_work_handler);

void dax_gesture_settings_attach(struct dax_gesture_model *attached, struct k_mutex *lock,
                                 void (*changed)(void)) {
    model = attached;
    model_lock = lock;
    model_changed = changed;
    memcpy(devicetree_slots, attached->slots, sizeof(devicetree_slots));
}

static int gesture_settings_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *changed = as_zmk_custom_setting_changed(eh);

    if (model == NULL || seeding) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    /* Settings are read from the work queue: this listener runs inside the write that raised it. */
    if (changed == NULL || zmk_custom_setting_matches(changed->setting, SUBSYSTEM_ID, NULL, NULL)) {
        k_work_submit(&refresh_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_gesture_settings, gesture_settings_listener);
ZMK_SUBSCRIPTION(dax_gesture_settings, zmk_custom_setting_changed);
ZMK_SUBSCRIPTION(dax_gesture_settings, zmk_custom_settings_initialized);
