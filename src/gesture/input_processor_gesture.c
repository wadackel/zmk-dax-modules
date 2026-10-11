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
#include "gesture_model.h"
#include "gesture_rpc.h"

LOG_MODULE_REGISTER(dax_gesture, CONFIG_ZMK_LOG_LEVEL);

struct gesture_config {
    uint32_t tap_ms;
    struct dax_gesture_model *model;
};

struct gesture_data {
    const struct device *dev;
    struct k_mutex lock;
    struct k_work_delayable idle_work;
    struct dax_gesture_state state;
    /* Index into the model's profiles, or DAX_GESTURE_NO_PROFILE. */
    uint8_t active;
    /* Recording a pattern for a client instead of recognizing. */
    bool capturing;
};

/* A capture ends once the drawing pauses this long, or if nothing is drawn at all for a while. */
#define CAPTURE_SETTLE_MS 700
#define CAPTURE_GIVE_UP_MS 15000

static bool layer_index_active(int index) {
    zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(index);

    return id != ZMK_KEYMAP_LAYER_ID_INVAL && zmk_keymap_layer_active(id);
}

static bool required_layers_active(const struct dax_gesture_profile *profile) {
    for (int index = 0; index < ZMK_KEYMAP_LAYERS_LEN; index++) {
        if ((profile->require_layers & BIT(index)) && !layer_index_active(index)) {
            return false;
        }
    }
    return true;
}

/* The profile of the highest active layer that has one; among several on that layer, the first
 * declared whose required layers are all active. */
static uint8_t select_profile(const struct dax_gesture_model *model) {
    for (int index = ZMK_KEYMAP_LAYERS_LEN - 1; index >= 0; index--) {
        if (!layer_index_active(index)) {
            continue;
        }
        for (size_t i = 0; i < model->profiles_len; i++) {
            const struct dax_gesture_profile *profile = &model->profiles[i];

            if ((profile->layers & BIT(index)) && required_layers_active(profile)) {
                return (uint8_t)i;
            }
        }
    }
    return DAX_GESTURE_NO_PROFILE;
}

static void fire(const struct gesture_config *cfg, uint8_t profile, int slot) {
    if (slot == DAX_GESTURE_NO_MATCH) {
        return;
    }
    dax_gesture_rpc_fired(profile, (uint8_t)slot);

    const struct zmk_behavior_binding binding = cfg->model->slots[slot].binding;
    struct zmk_behavior_binding_event event = {
        .position = INT32_MAX,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    if (zmk_behavior_queue_add(&event, binding, true, 0) < 0 ||
        zmk_behavior_queue_add(&event, binding, false, cfg->tap_ms) < 0) {
        LOG_WRN("Behavior queue full, gesture %d dropped", slot);
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
static uint8_t sync_profile(const struct device *dev, int64_t now_ms) {
    const struct gesture_config *cfg = dev->config;
    struct gesture_data *data = dev->data;
    const struct dax_gesture_model *model = cfg->model;
    uint8_t profile = select_profile(model);

    if (profile != data->active) {
        if (data->active != DAX_GESTURE_NO_PROFILE) {
            struct dax_gesture_set set = dax_gesture_model_set(model, data->active);

            fire(cfg, data->active, dax_gesture_end(&data->state, &model->tuning, &set, now_ms));
        }
        dax_gesture_reset(&data->state);
        arm_idle_timer(data, now_ms);
        data->active = profile;
    }
    return profile;
}

static void end_capture(struct gesture_data *data) {
    dax_gesture_rpc_capture_done(data->state.strokes);
    data->capturing = false;
    data->active = DAX_GESTURE_NO_PROFILE;
    dax_gesture_reset(&data->state);
    k_work_cancel_delayable(&data->idle_work);
}

static void idle_work_handler(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct gesture_data *data = CONTAINER_OF(delayable, struct gesture_data, idle_work);
    const struct gesture_config *cfg = data->dev->config;
    int64_t now_ms = k_uptime_get();

    k_mutex_lock(&data->lock, K_FOREVER);
    if (data->capturing) {
        end_capture(data);
    } else if (data->active != DAX_GESTURE_NO_PROFILE &&
               data->state.idle_deadline_ms != DAX_GESTURE_NO_DEADLINE) {
        struct dax_gesture_set set = dax_gesture_model_set(cfg->model, data->active);

        fire(cfg, data->active,
             dax_gesture_idle_elapsed(&data->state, &cfg->model->tuning, &set, now_ms));
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
    if (data->capturing) {
        int32_t value = event->value;

        event->value = 0;
        if (dax_gesture_capture_feed(&data->state, &cfg->model->tuning,
                                     event->code == INPUT_REL_X, value, now_ms,
                                     CAPTURE_SETTLE_MS)) {
            dax_gesture_rpc_capture_stroke(data->state.strokes);
        }
        arm_idle_timer(data, now_ms);
        k_mutex_unlock(&data->lock);
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint8_t profile = sync_profile(dev, now_ms);

    if (profile != DAX_GESTURE_NO_PROFILE) {
        struct dax_gesture_set set = dax_gesture_model_set(cfg->model, profile);
        int32_t value = event->value;
        uint8_t strokes_before = data->state.stroke_count;

        /* Zeroing reaches the host from a layer override as well; ZMK_INPUT_PROC_STOP would be
         * swallowed there. */
        if (cfg->model->profiles[profile].suppress_movement) {
            event->value = 0;
        }

        int slot = dax_gesture_feed(&data->state, &cfg->model->tuning, &set,
                                    event->code == INPUT_REL_X, value, now_ms);

        if (data->state.stroke_count > strokes_before) {
            dax_gesture_rpc_stroke(profile, data->state.strokes);
        }
        fire(cfg, profile, slot);
        arm_idle_timer(data, now_ms);
    }
    k_mutex_unlock(&data->lock);

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api gesture_driver_api = {
    .handle_event = gesture_handle_event,
};

/* The model is one per build, so runtime editing has a single thing to address. */
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "Exactly one dax,input-processor-gesture node is supported");

#define GESTURE_NODE DT_DRV_INST(0)
#define PROFILE_COUNT DT_CHILD_NUM_STATUS_OKAY(GESTURE_NODE)
#define SLOT_COUNT CONFIG_ZMK_DAX_GESTURE_MAX_GESTURES

#define LAYER_BIT(node, prop, idx) BIT(DT_PROP_BY_IDX(node, prop, idx))
#define LAYER_MASK(node, prop)                                                                     \
    COND_CODE_1(DT_NODE_HAS_PROP(node, prop), (DT_FOREACH_PROP_ELEM_SEP(node, prop, LAYER_BIT, (|))), (0))

#define STROKE_AT(node, prop, idx) ((dax_gesture_pattern_t)DT_PROP_BY_IDX(node, prop, idx) << (4 * (idx)))

#define PROFILE_ENTRY(profile)                                                                     \
    {                                                                                              \
        .layers = LAYER_MASK(profile, layers),                                                     \
        .require_layers = LAYER_MASK(profile, require_layers),                                     \
        .suppress_movement = DT_PROP(profile, suppress_movement),                                  \
    },

/* `ordinal` is the profile's position among the enabled ones, which is its index in the model. */
#define SLOT_ENTRY(gesture, ordinal)                                                               \
    {                                                                                              \
        .profile = ordinal,                                                                        \
        .pattern = (DT_FOREACH_PROP_ELEM_SEP(gesture, pattern, STROKE_AT, (|))),                   \
        .binding = ZMK_KEYMAP_EXTRACT_BINDING(0, gesture),                                         \
    },
#define GESTURE_CHECK(gesture, ...)                                                                \
    BUILD_ASSERT(DT_PROP_LEN(gesture, pattern) >= 1 &&                                             \
                     DT_PROP_LEN(gesture, pattern) <= DAX_GESTURE_MAX_STROKES,                     \
                 "A gesture pattern has one to eight strokes");                                    \
    BUILD_ASSERT(DT_PROP_LEN(gesture, bindings) == 1, "A gesture taps exactly one behavior");

/* The profile loop is itself a DT_FOREACH_CHILD_STATUS_OKAY expansion, and the preprocessor does
 * not re-enter a macro, so the gesture loops inside it use the _SEP_VARGS variant. */
#define PROFILE_ORDINAL(profile) (PROFILE_ORDINAL_OF_##profile)
#define PROFILE_SLOTS(profile)                                                                     \
    DT_FOREACH_CHILD_STATUS_OKAY_SEP_VARGS(profile, SLOT_ENTRY, (), PROFILE_ORDINAL(profile))
#define PROFILE_CHECKS(profile) DT_FOREACH_CHILD_STATUS_OKAY_SEP_VARGS(profile, GESTURE_CHECK, (), 0)

/* An enum numbers the enabled profiles in order without relying on child indexes being dense. */
#define PROFILE_ORDINAL_ENUM(profile) PROFILE_ORDINAL_OF_##profile,
enum { DT_FOREACH_CHILD_STATUS_OKAY(GESTURE_NODE, PROFILE_ORDINAL_ENUM) };

DT_FOREACH_CHILD_STATUS_OKAY(GESTURE_NODE, PROFILE_CHECKS)

static struct dax_gesture_profile gesture_profiles[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(GESTURE_NODE, PROFILE_ENTRY)};

static const struct dax_gesture_slot devicetree_slots[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(GESTURE_NODE, PROFILE_SLOTS)};
BUILD_ASSERT(ARRAY_SIZE(devicetree_slots) <= SLOT_COUNT,
             "More gestures than CONFIG_ZMK_DAX_GESTURE_MAX_GESTURES");

static struct dax_gesture_slot gesture_slots[SLOT_COUNT];
static dax_gesture_pattern_t gesture_views[PROFILE_COUNT * SLOT_COUNT];

static struct dax_gesture_model gesture_model = {
    .tuning =
        {
            .stroke_size = DT_INST_PROP(0, stroke_size),
            .movement_threshold = DT_INST_PROP(0, movement_threshold),
            .idle_timeout_ms = DT_INST_PROP(0, idle_timeout_ms),
            .partial_timeout_ms = DT_INST_PROP(0, partial_gesture_timeout_ms),
            .cooldown_ms = DT_INST_PROP(0, gesture_cooldown_ms),
            .eager = DT_INST_PROP(0, eager),
        },
    .profiles = gesture_profiles,
    .profiles_len = ARRAY_SIZE(gesture_profiles),
    .slots = gesture_slots,
    .slots_len = SLOT_COUNT,
    .views = gesture_views,
};

static const struct gesture_config gesture_config = {
    .tap_ms = DT_INST_PROP(0, tap_ms),
    .model = &gesture_model,
};
static struct gesture_data gesture_data;

#if IS_ENABLED(CONFIG_ZMK_DAX_GESTURE_RUNTIME)
/* A setting changed under the recognizer: what was being drawn no longer means the same. */
static void gesture_model_changed(void) {
    dax_gesture_reset(&gesture_data.state);
    k_work_cancel_delayable(&gesture_data.idle_work);
    gesture_data.active = DAX_GESTURE_NO_PROFILE;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_DAX_GESTURE_RPC)
void dax_gesture_capture_begin(void) {
    struct gesture_data *data = &gesture_data;
    int64_t now_ms = k_uptime_get();

    k_mutex_lock(&data->lock, K_FOREVER);
    dax_gesture_reset(&data->state);
    data->capturing = true;
    data->state.idle_deadline_ms = now_ms + CAPTURE_GIVE_UP_MS;
    arm_idle_timer(data, now_ms);
    k_mutex_unlock(&data->lock);
}

void dax_gesture_capture_cancel(void) {
    struct gesture_data *data = &gesture_data;

    k_mutex_lock(&data->lock, K_FOREVER);
    if (data->capturing) {
        dax_gesture_reset(&data->state);
        end_capture(data);
    }
    k_mutex_unlock(&data->lock);
}
#endif

static int gesture_init(const struct device *dev) {
    struct gesture_data *data = dev->data;

    data->dev = dev;
    data->active = DAX_GESTURE_NO_PROFILE;
    data->capturing = false;
    k_mutex_init(&data->lock);
    k_work_init_delayable(&data->idle_work, idle_work_handler);
    dax_gesture_init(&data->state);

    for (size_t i = 0; i < SLOT_COUNT; i++) {
        gesture_slots[i] = i < ARRAY_SIZE(devicetree_slots)
                               ? devicetree_slots[i]
                               : (struct dax_gesture_slot){.profile = DAX_GESTURE_NO_PROFILE};
    }
    dax_gesture_model_rebuild_views(&gesture_model);
#if IS_ENABLED(CONFIG_ZMK_DAX_GESTURE_RUNTIME)
    dax_gesture_settings_attach(&gesture_model, &data->lock, gesture_model_changed);
#endif
    return 0;
}

DEVICE_DT_INST_DEFINE(0, gesture_init, NULL, &gesture_data, &gesture_config, POST_KERNEL,
                      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &gesture_driver_api);

/* A layer changing without any movement still has to switch, and settle, the profile. */
static int gesture_layer_listener(const zmk_event_t *eh) {
    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct gesture_data *data = dev->data;

    if (as_zmk_layer_state_changed(eh) == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_mutex_lock(&data->lock, K_FOREVER);
    if (!data->capturing) {
        sync_profile(dev, k_uptime_get());
    }
    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_gesture, gesture_layer_listener);
ZMK_SUBSCRIPTION(dax_gesture, zmk_layer_state_changed);
