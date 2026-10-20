/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Gesture tuning, profiles and gestures as typed dax settings of the `dax_gesture` owner, so a
 * settings app can change them without a rebuild. The devicetree values are the defaults, and the
 * keys and their forms are listed in proto/dax/settings/settings.proto.
 */

#define DT_DRV_COMPAT dax_input_processor_gesture

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <dax/settings.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>

#include "gesture_model.h"

LOG_MODULE_DECLARE(dax_gesture, CONFIG_ZMK_LOG_LEVEL);

#define SUBSYSTEM_ID "dax_gesture"
#define GESTURE_NODE DT_DRV_INST(0)
#define SLOT_COUNT CONFIG_ZMK_DAX_GESTURE_MAX_GESTURES
#define PROFILE_COUNT DT_CHILD_NUM_STATUS_OKAY(GESTURE_NODE)

/*
 * A gesture slot, little-endian:
 *   [0]      profile index
 *   [1..4]   pattern, four bits per stroke
 *   [5..6]   behavior local id
 *   [7..10]  param1
 *   [11..14] param2
 * or the one byte SLOT_DELETED, which removes a gesture, devicetree default included. An unset
 * slot follows the devicetree.
 */
#define SLOT_DELETED 0xFE
#define SLOT_SIZE 15
#define PROFILE_NAME_SIZE 24

/* The subsystem these settings belong to is registered in gesture_rpc.c when Studio RPC is on;
 * a client needs it to tell that the keyboard has gestures. */

BUILD_ASSERT(PROFILE_COUNT >= 1, "Runtime gestures need at least one gesture profile");
BUILD_ASSERT(DT_CHILD_NUM(GESTURE_NODE) == DT_CHILD_NUM_STATUS_OKAY(GESTURE_NODE),
             "Remove a gesture profile instead of disabling it: setting slots follow child order");

static struct dax_gesture_model *model;
static struct k_mutex *model_lock;
static void (*model_changed)(void);
/* What the model held when it was attached: the devicetree's values, which are the defaults. */
static struct dax_gesture_tuning devicetree_tuning;
static struct dax_gesture_profile devicetree_profiles[PROFILE_COUNT];
static struct dax_gesture_slot devicetree_slots[SLOT_COUNT];

static int put_int32(int32_t value, uint8_t *buf, size_t size) {
    if (size < sizeof(int32_t)) {
        return -ENOSPC;
    }
    sys_put_le32(value, buf);
    return sizeof(int32_t);
}

static int put_bool(bool value, uint8_t *buf, size_t size) {
    if (size < 1) {
        return -ENOSPC;
    }
    buf[0] = value;
    return 1;
}

/* The defaults are copied from the model when it is attached at boot, before settings load. */
#define TUNING_DEFAULT(name, put, field)                                                           \
    static int name(uint16_t slot, uint8_t *buf, size_t size) {                                    \
        ARG_UNUSED(slot);                                                                          \
        return model ? put(devicetree_tuning.field, buf, size) : -ENOENT;                         \
    }
#define PROFILE_DEFAULT(name, put, field)                                                          \
    static int name(uint16_t slot, uint8_t *buf, size_t size) {                                    \
        return model ? put(devicetree_profiles[slot].field, buf, size) : -ENOENT;                 \
    }

TUNING_DEFAULT(stroke_size_default, put_int32, stroke_size)
TUNING_DEFAULT(idle_ms_default, put_int32, idle_timeout_ms)
TUNING_DEFAULT(cooldown_ms_default, put_int32, cooldown_ms)
TUNING_DEFAULT(partial_ms_default, put_int32, partial_timeout_ms)
TUNING_DEFAULT(eager_default, put_bool, eager)
PROFILE_DEFAULT(layers_default, put_int32, layers)
PROFILE_DEFAULT(require_default, put_int32, require_layers)
PROFILE_DEFAULT(suppress_default, put_bool, suppress_movement)

#define PROFILE_NAME(profile) DT_NODE_FULL_NAME(profile)
#define PROFILE_NAME_CHECK(profile)                                                                \
    BUILD_ASSERT(sizeof(DT_NODE_FULL_NAME(profile)) - 1 <= PROFILE_NAME_SIZE,                      \
                 "A gesture profile's node name must fit PROFILE_NAME_SIZE");
DT_FOREACH_CHILD_STATUS_OKAY(GESTURE_NODE, PROFILE_NAME_CHECK)
static const char *const profile_names[] = {
    DT_FOREACH_CHILD_STATUS_OKAY_SEP(GESTURE_NODE, PROFILE_NAME, (, ))};

static int name_default(uint16_t slot, uint8_t *buf, size_t size) {
    size_t len = strlen(profile_names[slot]);
    if (len > size) {
        return -ENOSPC;
    }
    memcpy(buf, profile_names[slot], len);
    return len;
}

static int gesture_default(uint16_t slot, uint8_t *buf, size_t size) {
    const struct dax_gesture_slot *gesture = &devicetree_slots[slot];
    if (!model || gesture->profile == DAX_GESTURE_NO_PROFILE) {
        return -ENOENT;
    }
    if (size < SLOT_SIZE) {
        return -ENOSPC;
    }
    buf[0] = gesture->profile;
    sys_put_le32(gesture->pattern, &buf[1]);
    sys_put_le16(zmk_behavior_get_local_id(gesture->binding.behavior_dev), &buf[5]);
    sys_put_le32(gesture->binding.param1, &buf[7]);
    sys_put_le32(gesture->binding.param2, &buf[11]);
    return SLOT_SIZE;
}

/* A gesture the recognizer can fire, checked as a keymap or combo binding is, or the one byte
 * that removes one. A stored gesture whose behavior this build lacks reads as unreadable. */
static int gesture_validate(uint16_t slot, const uint8_t *data, size_t len) {
    ARG_UNUSED(slot);
    if (len == 1) {
        return data[0] == SLOT_DELETED ? 0 : -EINVAL;
    }
    if (len != SLOT_SIZE || data[0] >= PROFILE_COUNT ||
        !dax_gesture_pattern_is_valid(sys_get_le32(&data[1]))) {
        return -EINVAL;
    }
    struct zmk_behavior_binding binding = {
        .behavior_dev = zmk_behavior_find_behavior_name_from_local_id(sys_get_le16(&data[5])),
        .param1 = sys_get_le32(&data[7]),
        .param2 = sys_get_le32(&data[11]),
    };
    if (!binding.behavior_dev || zmk_behavior_validate_binding(&binding) < 0) {
        return -EINVAL;
    }
    return 0;
}

#define NUMBER_TYPE(name, _min, _max, _default)                                                    \
    static const struct dax_setting_type name = {                                                  \
        .kind = DAX_SETTING_INT32, .min = _min, .max = _max, .default_value = _default}
#define FLAG_TYPE(name, _default)                                                                  \
    static const struct dax_setting_type name = {.kind = DAX_SETTING_BOOL,                        \
                                                 .default_value = _default}

NUMBER_TYPE(stroke_size_type, 10, 10000, stroke_size_default);
NUMBER_TYPE(idle_ms_type, 0, 10000, idle_ms_default);
NUMBER_TYPE(cooldown_ms_type, 0, 10000, cooldown_ms_default);
NUMBER_TYPE(partial_ms_type, 0, 10000, partial_ms_default);
FLAG_TYPE(eager_type, eager_default);
NUMBER_TYPE(layers_type, 0, INT32_MAX, layers_default);
NUMBER_TYPE(require_type, 0, INT32_MAX, require_default);
FLAG_TYPE(suppress_type, suppress_default);
static const struct dax_setting_type name_type = {
    .kind = DAX_SETTING_BYTES, .read_only = true, .default_value = name_default};
static const struct dax_setting_type gesture_type = {
    .kind = DAX_SETTING_BYTES, .default_value = gesture_default, .validate = gesture_validate};

#define TUNING_SETTING(name, key, size, type)                                                      \
    DAX_SETTING_DEFINE_TYPED(name, SUBSYSTEM_ID, key, size, &type)
#define PROFILE_SETTING(name, key, size, type)                                                     \
    DAX_SETTING_DEFINE_TYPED_SLOTS(name, SUBSYSTEM_ID, key, PROFILE_COUNT, size, &type)

TUNING_SETTING(dax_gesture_stroke_size, "stroke_size", sizeof(int32_t), stroke_size_type);
TUNING_SETTING(dax_gesture_idle_ms, "idle_ms", sizeof(int32_t), idle_ms_type);
TUNING_SETTING(dax_gesture_cooldown_ms, "cooldown_ms", sizeof(int32_t), cooldown_ms_type);
TUNING_SETTING(dax_gesture_partial_ms, "partial_ms", sizeof(int32_t), partial_ms_type);
TUNING_SETTING(dax_gesture_eager, "eager", 1, eager_type);
PROFILE_SETTING(dax_gesture_profile_name, "profile_name", PROFILE_NAME_SIZE, name_type);
PROFILE_SETTING(dax_gesture_profile_layers, "profile_layers", sizeof(int32_t), layers_type);
PROFILE_SETTING(dax_gesture_profile_require, "profile_require", sizeof(int32_t), require_type);
PROFILE_SETTING(dax_gesture_profile_suppress, "profile_suppress", 1, suppress_type);
DAX_SETTING_DEFINE_TYPED_SLOTS(dax_gesture_slot, SUBSYSTEM_ID, "gesture", SLOT_COUNT, SLOT_SIZE,
                               &gesture_type);

static struct dax_gesture_slot read_slot(uint16_t index) {
    uint8_t value[SLOT_SIZE];
    int len = dax_setting_read(&dax_gesture_slot, index, value, sizeof(value), NULL);
    const char *behavior =
        len == SLOT_SIZE ? zmk_behavior_find_behavior_name_from_local_id(sys_get_le16(&value[5]))
                         : NULL;
    /* A removed or free slot: nothing to tap. */
    if (!behavior) {
        return (struct dax_gesture_slot){.profile = DAX_GESTURE_NO_PROFILE};
    }
    return (struct dax_gesture_slot){
        .profile = value[0],
        .pattern = sys_get_le32(&value[1]),
        .binding = {.behavior_dev = behavior,
                    .param1 = sys_get_le32(&value[7]),
                    .param2 = sys_get_le32(&value[11])},
    };
}

static void load_model(void) {
    struct dax_gesture_tuning *tuning = &model->tuning;
    tuning->stroke_size = dax_setting_read_int32(&dax_gesture_stroke_size, 0, tuning->stroke_size);
    tuning->idle_timeout_ms =
        dax_setting_read_int32(&dax_gesture_idle_ms, 0, tuning->idle_timeout_ms);
    tuning->cooldown_ms = dax_setting_read_int32(&dax_gesture_cooldown_ms, 0, tuning->cooldown_ms);
    tuning->partial_timeout_ms =
        dax_setting_read_int32(&dax_gesture_partial_ms, 0, tuning->partial_timeout_ms);
    tuning->eager = dax_setting_read_bool(&dax_gesture_eager, 0, tuning->eager);

    for (uint16_t i = 0; i < model->profiles_len; i++) {
        struct dax_gesture_profile *profile = &model->profiles[i];
        profile->layers = dax_setting_read_int32(&dax_gesture_profile_layers, i, profile->layers);
        profile->require_layers =
            dax_setting_read_int32(&dax_gesture_profile_require, i, profile->require_layers);
        profile->suppress_movement =
            dax_setting_read_bool(&dax_gesture_profile_suppress, i, profile->suppress_movement);
    }

    size_t used = 0;
    for (uint16_t i = 0; i < SLOT_COUNT; i++) {
        model->slots[i] = read_slot(i);
        used += model->slots[i].profile != DAX_GESTURE_NO_PROFILE;
    }
    dax_gesture_model_rebuild_views(model);

    LOG_DBG("Loaded %u gestures in %u profiles from settings", (unsigned int)used,
            (unsigned int)model->profiles_len);
}

static void refresh_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    k_mutex_lock(model_lock, K_FOREVER);
    load_model();
    model_changed();
    k_mutex_unlock(model_lock);
}

static K_WORK_DEFINE(refresh_work, refresh_work_handler);

void dax_gesture_settings_attach(struct dax_gesture_model *attached, struct k_mutex *lock,
                                 void (*changed)(void)) {
    __ASSERT(attached->profiles_len == PROFILE_COUNT && attached->slots_len == SLOT_COUNT,
             "the gesture model and its settings disagree on the number of slots");
    model_lock = lock;
    model_changed = changed;
    devicetree_tuning = attached->tuning;
    memcpy(devicetree_profiles, attached->profiles, sizeof(devicetree_profiles));
    memcpy(devicetree_slots, attached->slots, sizeof(devicetree_slots));
    model = attached;
}

static int gesture_settings_listener(const zmk_event_t *eh) {
    const struct dax_setting_changed *changed = as_dax_setting_changed(eh);

    if (model == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    /* Settings are read from the work queue: this listener runs inside the write that raised it. */
    if (changed == NULL || strcmp(changed->owner, SUBSYSTEM_ID) == 0) {
        k_work_submit(&refresh_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_gesture_settings, gesture_settings_listener);
ZMK_SUBSCRIPTION(dax_gesture_settings, dax_setting_changed);
ZMK_SUBSCRIPTION(dax_gesture_settings, dax_settings_loaded);
