/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * The dax pointer processor. The order of the steps, the rotation and the rules for the held
 * layer are adapted from cormoran's zmk-module-runtime-input-processor (MIT), without its
 * inertia, axis snap and temporary-config behaviors.
 */

#define DT_DRV_COMPAT dax_input_processor_pointer

#include <math.h>

#include <drivers/input_processor.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <dax/pointer.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/keys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct pointer_config {
    const char *name;
    const uint16_t *x_codes;
    const uint16_t *y_codes;
    size_t codes_len;
    struct dax_pointer_values defaults;
};

struct pointer_data {
    struct k_spinlock lock;
    /* Under `lock`: written by dax_pointer_apply, read by each report. */
    struct dax_pointer_values values;
    int32_t cos_1000;
    int32_t sin_1000;

    /* The input thread's own: rotation pairs an X report with the Y report next to it. */
    int32_t last_x;
    int32_t last_y;
    bool has_x;
    bool has_y;

    struct k_work_delayable activate_work;
    struct k_work_delayable deactivate_work;
    int64_t last_keypress_ms;
    bool layer_active;
    /* The id switched on, so the same layer goes off even if the keymap was reordered since. */
    zmk_keymap_layer_id_t active_layer_id;
};

#define POINTER_DEVICE(n) DEVICE_DT_INST_GET(n),
static const struct device *const pointers[] = {DT_INST_FOREACH_STATUS_OKAY(POINTER_DEVICE)};

BUILD_ASSERT(ARRAY_SIZE(pointers) == DAX_POINTER_COUNT, "one slot per pointer processor");

static int32_t cos_1000_of(int32_t degrees) {
    return (int32_t)lround(cos((double)degrees * 3.14159265358979 / 180.0) * 1000.0);
}

static int32_t sin_1000_of(int32_t degrees) {
    return (int32_t)lround(sin((double)degrees * 3.14159265358979 / 180.0) * 1000.0);
}

static int code_index(uint16_t code, const uint16_t *codes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (codes[i] == code) {
            return i;
        }
    }
    return -ENODEV;
}

static bool on_active_layer(uint32_t layers) {
    if (layers == 0) {
        return true;
    }
    for (zmk_keymap_layer_index_t index = 0; index < ZMK_KEYMAP_LAYERS_LEN && index < 32;
         index++) {
        if (!(layers & BIT(index))) {
            continue;
        }
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(index);
        if (id != ZMK_KEYMAP_LAYER_ID_INVAL && zmk_keymap_layer_active(id)) {
            return true;
        }
    }
    return false;
}

static void layer_off(struct pointer_data *data) {
    if (!data->layer_active) {
        return;
    }
    data->layer_active = false;
    int ret = zmk_keymap_layer_deactivate(data->active_layer_id, false);
    if (ret < 0) {
        LOG_WRN("Pointer could not turn layer %u off: %d", data->active_layer_id, ret);
    }
}

static void activate_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct pointer_data *data = CONTAINER_OF(dwork, struct pointer_data, activate_work);

    if (data->layer_active) {
        return;
    }
    uint8_t position = UINT8_MAX;
    uint16_t back_ms = 0;
    K_SPINLOCK(&data->lock) {
        position = data->values.temp_layer_on ? data->values.temp_layer : UINT8_MAX;
        back_ms = data->values.temp_layer_back_ms;
    }
    zmk_keymap_layer_id_t id =
        position < ZMK_KEYMAP_LAYERS_LEN ? zmk_keymap_layer_index_to_id(position)
                                         : ZMK_KEYMAP_LAYER_ID_INVAL;
    /* A position past the keymap, or of a removed layer: no layer to hold. */
    if (id == ZMK_KEYMAP_LAYER_ID_INVAL) {
        return;
    }
    int ret = zmk_keymap_layer_activate(id, false);
    if (ret < 0) {
        LOG_WRN("Pointer could not turn layer %u on: %d", id, ret);
        return;
    }
    data->layer_active = true;
    data->active_layer_id = id;
    /* The report that asked for the layer saw it still off and scheduled no way back. */
    k_work_reschedule(&data->deactivate_work, K_MSEC(back_ms));
}

static void deactivate_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    layer_off(CONTAINER_OF(dwork, struct pointer_data, deactivate_work));
}

/* In 32 bits a remainder up to the divisor and a value that an earlier processor took past
 * 16 bits would overflow; the result is saturated instead. */
static void scale(struct input_event *event, int32_t mul, int32_t div,
                  struct zmk_input_processor_state *state) {
    int64_t value = (int64_t)event->value * mul;
    if (state && state->remainder) {
        value += *state->remainder;
    }
    int64_t scaled = value / div;
    if (state && state->remainder) {
        *state->remainder = (int16_t)(value - scaled * div);
    }
    event->value = CLAMP(scaled, INT16_MIN, INT16_MAX);
}

static int pointer_handle_event(const struct device *dev, struct input_event *event,
                                uint32_t param1, uint32_t param2,
                                struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    const struct pointer_config *cfg = dev->config;
    struct pointer_data *data = dev->data;

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    int x_index = code_index(event->code, cfg->x_codes, cfg->codes_len);
    int y_index = code_index(event->code, cfg->y_codes, cfg->codes_len);
    if (x_index < 0 && y_index < 0) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct dax_pointer_values values;
    int32_t cos_1000 = 1000;
    int32_t sin_1000 = 0;
    K_SPINLOCK(&data->lock) {
        values = data->values;
        cos_1000 = data->cos_1000;
        sin_1000 = data->sin_1000;
    }
    if (!on_active_layer(values.layers)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    bool is_x = x_index >= 0;
    if (values.xy_to_scroll) {
        event->code = is_x ? INPUT_REL_HWHEEL : INPUT_REL_WHEEL;
    } else if (values.xy_swap) {
        event->code = is_x ? INPUT_REL_Y : INPUT_REL_X;
    }

    if (values.temp_layer_on && event->value != 0 && !data->layer_active &&
        k_uptime_get() - data->last_keypress_ms >= values.temp_layer_after_ms) {
        k_work_reschedule(&data->activate_work, K_NO_WAIT);
    }

    if (values.rotation != 0) {
        int32_t value = event->value;
        if (is_x) {
            data->last_x = value;
            data->has_x = true;
            event->value = data->has_y ? (int32_t)(((int64_t)data->last_x * cos_1000 -
                                                    (int64_t)data->last_y * sin_1000) /
                                                   1000)
                                       : 0;
            data->has_y = false;
        } else {
            data->last_y = value;
            data->has_y = true;
            event->value = data->has_x ? (int32_t)(((int64_t)data->last_x * sin_1000 +
                                                    (int64_t)data->last_y * cos_1000) /
                                                   1000)
                                       : 0;
            data->has_x = false;
        }
    }

    if (is_x ? values.x_invert : values.y_invert) {
        event->value = -event->value;
    }

    scale(event, values.scale_mul, values.scale_div, state);

    if (values.temp_layer_on && data->layer_active) {
        k_work_reschedule(&data->deactivate_work, K_MSEC(values.temp_layer_back_ms));
    }
    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api pointer_driver_api = {
    .handle_event = pointer_handle_event,
};

void dax_pointer_defaults(uint16_t slot, struct dax_pointer_values *values) {
    const struct pointer_config *cfg = pointers[slot]->config;
    *values = cfg->defaults;
}

const char *dax_pointer_name(uint16_t slot) {
    const struct pointer_config *cfg = pointers[slot]->config;
    return cfg->name;
}

void dax_pointer_apply(uint16_t slot, const struct dax_pointer_values *values) {
    struct pointer_data *data = pointers[slot]->data;

    int32_t cos_1000 = cos_1000_of(values->rotation);
    int32_t sin_1000 = sin_1000_of(values->rotation);

    k_work_cancel_delayable(&data->activate_work);
    k_work_cancel_delayable(&data->deactivate_work);
    K_SPINLOCK(&data->lock) {
        data->values = *values;
        data->cos_1000 = cos_1000;
        data->sin_1000 = sin_1000;
    }
    /* The held layer may no longer be the one the values name. */
    layer_off(data);
}

static int pointer_init(const struct device *dev) {
    const struct pointer_config *cfg = dev->config;
    struct pointer_data *data = dev->data;

    /* settings_load() runs later, from main(), and reports can come first. */
    data->values = cfg->defaults;
    data->cos_1000 = cos_1000_of(cfg->defaults.rotation);
    data->sin_1000 = sin_1000_of(cfg->defaults.rotation);
    k_work_init_delayable(&data->activate_work, activate_work_handler);
    k_work_init_delayable(&data->deactivate_work, deactivate_work_handler);
    return 0;
}

/* A keymap that never names &trans or &kp drops the node, and then no key can be bound to it. */
static bool is_transparent(const struct zmk_behavior_binding *binding) {
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(trans))
    return zmk_behavior_get_binding(binding->behavior_dev) ==
           DEVICE_DT_GET(DT_NODELABEL(trans));
#else
    return false;
#endif
}

static bool plays_modifier(const struct zmk_behavior_binding *binding) {
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(kp))
    if (zmk_behavior_get_binding(binding->behavior_dev) != DEVICE_DT_GET(DT_NODELABEL(kp))) {
        return false;
    }
    uint16_t page = ZMK_HID_USAGE_PAGE(binding->param1);
    return is_mod(page ? page : HID_USAGE_KEY, ZMK_HID_USAGE_ID(binding->param1));
#else
    return false;
#endif
}

/* What the key does with the layers that are on now: the binding of the highest one that is not
 * transparent there, or NULL. */
static const struct zmk_behavior_binding *resolved_binding(uint32_t position) {
    for (int index = ZMK_KEYMAP_LAYERS_LEN - 1; index >= 0; index--) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(index);
        if (id == ZMK_KEYMAP_LAYER_ID_INVAL || !zmk_keymap_layer_active(id)) {
            continue;
        }
        const struct zmk_behavior_binding *binding =
            zmk_keymap_get_layer_binding_at_idx(id, position);
        if (binding && !is_transparent(binding)) {
            return binding;
        }
    }
    return NULL;
}

/* A key bound on the held layer belongs to it, and a modifier is held for the movement. Any other
 * key ends the layer, a combo's or a sensor's included: their positions lie past the keymap, so
 * no layer binds them. */
static bool keeps_layer(zmk_keymap_layer_id_t layer, uint32_t position) {
    const struct zmk_behavior_binding *held = zmk_keymap_get_layer_binding_at_idx(layer, position);
    if (held && !is_transparent(held)) {
        return true;
    }
    const struct zmk_behavior_binding *binding = resolved_binding(position);
    return binding && plays_modifier(binding);
}

static int pointer_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (size_t i = 0; i < ARRAY_SIZE(pointers); i++) {
        struct pointer_data *data = pointers[i]->data;
        if (data->layer_active && !keeps_layer(data->active_layer_id, ev->position)) {
            k_work_cancel_delayable(&data->deactivate_work);
            layer_off(data);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int pointer_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    int64_t now = k_uptime_get();
    for (size_t i = 0; i < ARRAY_SIZE(pointers); i++) {
        struct pointer_data *data = pointers[i]->data;
        data->last_keypress_ms = now;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_pointer_position, pointer_position_listener);
ZMK_SUBSCRIPTION(dax_pointer_position, zmk_position_state_changed);
ZMK_LISTENER(dax_pointer_keycode, pointer_keycode_listener);
ZMK_SUBSCRIPTION(dax_pointer_keycode, zmk_keycode_state_changed);

#define CHECK_RANGE(n, prop, min, max)                                                             \
    BUILD_ASSERT(DT_INST_PROP(n, prop) >= (min) && DT_INST_PROP(n, prop) <= (max),                 \
                 #prop " is out of range")

#define POINTER_INST(n)                                                                            \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, x_codes) == DT_INST_PROP_LEN(n, y_codes),                     \
                 "x-codes and y-codes need the same length");                                      \
    BUILD_ASSERT(sizeof(DT_INST_PROP(n, display_name)) - 1 <= DAX_POINTER_NAME_SIZE,               \
                 "display-name must fit DAX_POINTER_NAME_SIZE");                                   \
    CHECK_RANGE(n, scale_multiplier, 1, 65535);                                                    \
    CHECK_RANGE(n, scale_divisor, 1, 32767);                                                       \
    CHECK_RANGE(n, rotation_degrees, -360, 360);                                                   \
    CHECK_RANGE(n, active_layers, 0, INT32_MAX);                                                   \
    CHECK_RANGE(n, temp_layer, 0, ZMK_KEYMAP_LAYERS_LEN - 1);                                      \
    CHECK_RANGE(n, temp_layer_activation_delay_ms, 0, 60000);                                      \
    CHECK_RANGE(n, temp_layer_deactivation_delay_ms, 0, 60000);                                    \
    static const uint16_t pointer_x_codes_##n[] = DT_INST_PROP(n, x_codes);                        \
    static const uint16_t pointer_y_codes_##n[] = DT_INST_PROP(n, y_codes);                        \
    static const struct pointer_config pointer_config_##n = {                                      \
        .name = DT_INST_PROP(n, display_name),                                                     \
        .x_codes = pointer_x_codes_##n,                                                            \
        .y_codes = pointer_y_codes_##n,                                                            \
        .codes_len = DT_INST_PROP_LEN(n, x_codes),                                                 \
        .defaults =                                                                                \
            {                                                                                      \
                .scale_mul = DT_INST_PROP(n, scale_multiplier),                                    \
                .scale_div = DT_INST_PROP(n, scale_divisor),                                       \
                .rotation = DT_INST_PROP(n, rotation_degrees),                                     \
                .xy_swap = DT_INST_PROP(n, xy_swap),                                               \
                .x_invert = DT_INST_PROP(n, x_invert),                                             \
                .y_invert = DT_INST_PROP(n, y_invert),                                             \
                .xy_to_scroll = DT_INST_PROP(n, xy_to_scroll),                                     \
                .layers = DT_INST_PROP(n, active_layers),                                          \
                .temp_layer_on = DT_INST_PROP(n, temp_layer_enabled),                              \
                .temp_layer = DT_INST_PROP(n, temp_layer),                                         \
                .temp_layer_after_ms = DT_INST_PROP(n, temp_layer_activation_delay_ms),            \
                .temp_layer_back_ms = DT_INST_PROP(n, temp_layer_deactivation_delay_ms),           \
            },                                                                                     \
    };                                                                                             \
    static struct pointer_data pointer_data_##n;                                                   \
    DEVICE_DT_INST_DEFINE(n, pointer_init, NULL, &pointer_data_##n, &pointer_config_##n,           \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &pointer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(POINTER_INST)
