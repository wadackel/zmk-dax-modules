/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT dax_input_processor_gesture

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/input_processor.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include "gesture_core.h"

LOG_MODULE_REGISTER(dax_gesture, CONFIG_ZMK_LOG_LEVEL);

struct gesture_profile {
    uint32_t layers;
    uint32_t require_layers;
    bool suppress_movement;
    struct dax_gesture_set set;
    const struct zmk_behavior_binding *bindings;
};

struct gesture_config {
    struct dax_gesture_tuning tuning;
    uint32_t tap_ms;
    const struct gesture_profile *profiles;
    size_t profiles_len;
};

struct gesture_data {
    const struct device *dev;
    struct k_mutex lock;
    struct k_work_delayable idle_work;
    struct dax_gesture_state state;
    const struct gesture_profile *active;
};

static bool layer_index_active(int index) {
    zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(index);

    return id != ZMK_KEYMAP_LAYER_ID_INVAL && zmk_keymap_layer_active(id);
}

static bool required_layers_active(const struct gesture_profile *profile) {
    for (int index = 0; index < ZMK_KEYMAP_LAYERS_LEN; index++) {
        if ((profile->require_layers & BIT(index)) && !layer_index_active(index)) {
            return false;
        }
    }
    return true;
}

/* The profile of the highest active layer that has one; among several on that layer, the first
 * declared whose required layers are all active. */
static const struct gesture_profile *select_profile(const struct gesture_config *cfg) {
    for (int index = ZMK_KEYMAP_LAYERS_LEN - 1; index >= 0; index--) {
        if (!layer_index_active(index)) {
            continue;
        }
        for (size_t i = 0; i < cfg->profiles_len; i++) {
            const struct gesture_profile *profile = &cfg->profiles[i];

            if ((profile->layers & BIT(index)) && required_layers_active(profile)) {
                return profile;
            }
        }
    }
    return NULL;
}

static void fire(const struct gesture_config *cfg, const struct gesture_profile *profile,
                 int index) {
    if (index == DAX_GESTURE_NO_MATCH) {
        return;
    }

    struct zmk_behavior_binding_event event = {
        .position = INT32_MAX,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    if (zmk_behavior_queue_add(&event, profile->bindings[index], true, 0) < 0 ||
        zmk_behavior_queue_add(&event, profile->bindings[index], false, cfg->tap_ms) < 0) {
        LOG_WRN("Behavior queue full, gesture %d dropped", index);
    }
}

static void arm_idle_timer(struct gesture_data *data, int64_t now_ms) {
    if (data->state.idle_deadline_ms == DAX_GESTURE_NO_DEADLINE) {
        k_work_cancel_delayable(&data->idle_work);
        return;
    }
    k_work_reschedule(&data->idle_work, K_MSEC(MAX(data->state.idle_deadline_ms - now_ms, 0)));
}

/* Switches to whichever profile applies now. Leaving a profile settles what was drawn under it,
 * which is how a pattern fires on key release when not in eager mode. */
static const struct gesture_profile *sync_profile(const struct device *dev, int64_t now_ms) {
    const struct gesture_config *cfg = dev->config;
    struct gesture_data *data = dev->data;
    const struct gesture_profile *profile = select_profile(cfg);

    if (profile != data->active) {
        if (data->active != NULL) {
            fire(cfg, data->active,
                 dax_gesture_end(&data->state, &cfg->tuning, &data->active->set, now_ms));
        }
        dax_gesture_reset(&data->state);
        arm_idle_timer(data, now_ms);
        data->active = profile;
    }
    return profile;
}

static void idle_work_handler(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct gesture_data *data = CONTAINER_OF(delayable, struct gesture_data, idle_work);
    const struct gesture_config *cfg = data->dev->config;
    int64_t now_ms = k_uptime_get();

    k_mutex_lock(&data->lock, K_FOREVER);
    if (data->active != NULL && data->state.idle_deadline_ms != DAX_GESTURE_NO_DEADLINE) {
        fire(cfg, data->active,
             dax_gesture_idle_elapsed(&data->state, &cfg->tuning, &data->active->set, now_ms));
    }
    k_mutex_unlock(&data->lock);
}

static int gesture_handle_event(const struct device *dev, struct input_event *event,
                                uint32_t param1, uint32_t param2,
                                struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    if (event->type != INPUT_EV_REL ||
        (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const struct gesture_config *cfg = dev->config;
    struct gesture_data *data = dev->data;
    int64_t now_ms = k_uptime_get();

    k_mutex_lock(&data->lock, K_FOREVER);
    const struct gesture_profile *profile = sync_profile(dev, now_ms);

    if (profile != NULL) {
        int32_t value = event->value;

        /* Zeroing reaches the host from a layer override as well; ZMK_INPUT_PROC_STOP would be
         * swallowed there. */
        if (profile->suppress_movement) {
            event->value = 0;
        }
        fire(cfg, profile,
             dax_gesture_feed(&data->state, &cfg->tuning, &profile->set,
                              event->code == INPUT_REL_X, value, now_ms));
        arm_idle_timer(data, now_ms);
    }
    k_mutex_unlock(&data->lock);

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api gesture_driver_api = {
    .handle_event = gesture_handle_event,
};

static int gesture_init(const struct device *dev) {
    struct gesture_data *data = dev->data;

    data->dev = dev;
    data->active = NULL;
    k_mutex_init(&data->lock);
    k_work_init_delayable(&data->idle_work, idle_work_handler);
    dax_gesture_init(&data->state);
    return 0;
}

#define LAYER_BIT(node, prop, idx) BIT(DT_PROP_BY_IDX(node, prop, idx))
#define LAYER_MASK(node, prop)                                                                     \
    COND_CODE_1(DT_NODE_HAS_PROP(node, prop), (DT_FOREACH_PROP_ELEM_SEP(node, prop, LAYER_BIT, (|))), (0))

#define STROKE_AT(node, prop, idx) ((dax_gesture_pattern_t)DT_PROP_BY_IDX(node, prop, idx) << (4 * (idx)))
#define GESTURE_PATTERN(gesture) (DT_FOREACH_PROP_ELEM_SEP(gesture, pattern, STROKE_AT, (|)))
#define GESTURE_BINDING(gesture) ZMK_KEYMAP_EXTRACT_BINDING(0, gesture)
#define GESTURE_CHECK(gesture)                                                                     \
    BUILD_ASSERT(DT_PROP_LEN(gesture, pattern) >= 1 &&                                             \
                     DT_PROP_LEN(gesture, pattern) <= DAX_GESTURE_MAX_STROKES,                     \
                 "A gesture pattern has one to eight strokes");                                    \
    BUILD_ASSERT(DT_PROP_LEN(gesture, bindings) == 1, "A gesture taps exactly one behavior");

/* The profile loop is itself a DT_FOREACH_CHILD_STATUS_OKAY expansion, and the preprocessor does
 * not re-enter a macro, so the gesture loops inside it use the _SEP variant throughout. */
#define PROFILE_TABLES(profile)                                                                    \
    DT_FOREACH_CHILD_STATUS_OKAY_SEP(profile, GESTURE_CHECK, ())                                   \
    static const dax_gesture_pattern_t _CONCAT(gesture_patterns_, profile)[] = {                   \
        DT_FOREACH_CHILD_STATUS_OKAY_SEP(profile, GESTURE_PATTERN, (, ))};                         \
    static const struct zmk_behavior_binding _CONCAT(gesture_bindings_, profile)[] = {             \
        DT_FOREACH_CHILD_STATUS_OKAY_SEP(profile, GESTURE_BINDING, (, ))};

#define PROFILE_ENTRY(profile)                                                                     \
    {                                                                                              \
        .layers = LAYER_MASK(profile, layers),                                                     \
        .require_layers = LAYER_MASK(profile, require_layers),                                     \
        .suppress_movement = DT_PROP(profile, suppress_movement),                                  \
        .set = {.patterns = _CONCAT(gesture_patterns_, profile),                                   \
                .count = ARRAY_SIZE(_CONCAT(gesture_patterns_, profile))},                         \
        .bindings = _CONCAT(gesture_bindings_, profile),                                           \
    }

#define GESTURE_INST(n)                                                                            \
    DT_INST_FOREACH_CHILD_STATUS_OKAY(n, PROFILE_TABLES)                                           \
    static const struct gesture_profile gesture_profiles_##n[] = {                                 \
        DT_INST_FOREACH_CHILD_STATUS_OKAY_SEP(n, PROFILE_ENTRY, (, ))};                            \
    static const struct gesture_config gesture_config_##n = {                                      \
        .tuning =                                                                                  \
            {                                                                                      \
                .stroke_size = DT_INST_PROP(n, stroke_size),                                       \
                .movement_threshold = DT_INST_PROP(n, movement_threshold),                         \
                .idle_timeout_ms = DT_INST_PROP(n, idle_timeout_ms),                               \
                .partial_timeout_ms = DT_INST_PROP(n, partial_gesture_timeout_ms),                 \
                .cooldown_ms = DT_INST_PROP(n, gesture_cooldown_ms),                               \
                .eager = DT_INST_PROP(n, eager),                                                   \
            },                                                                                     \
        .tap_ms = DT_INST_PROP(n, tap_ms),                                                         \
        .profiles = gesture_profiles_##n,                                                          \
        .profiles_len = ARRAY_SIZE(gesture_profiles_##n),                                          \
    };                                                                                             \
    static struct gesture_data gesture_data_##n;                                                   \
    DEVICE_DT_INST_DEFINE(n, gesture_init, NULL, &gesture_data_##n, &gesture_config_##n,           \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &gesture_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GESTURE_INST)

#define GESTURE_DEVICE(n) DEVICE_DT_INST_GET(n),

/* A layer changing without any movement still has to switch, and settle, the profile. */
static int gesture_layer_listener(const zmk_event_t *eh) {
    static const struct device *const devices[] = {DT_INST_FOREACH_STATUS_OKAY(GESTURE_DEVICE)};

    if (as_zmk_layer_state_changed(eh) == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
        struct gesture_data *data = devices[i]->data;

        k_mutex_lock(&data->lock, K_FOREVER);
        sync_profile(devices[i], k_uptime_get());
        k_mutex_unlock(&data->lock);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_gesture, gesture_layer_listener);
ZMK_SUBSCRIPTION(dax_gesture, zmk_layer_state_changed);
