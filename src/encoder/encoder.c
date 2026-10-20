/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * The dax encoder behavior. Each step of a sensor plays a binding, from a `dax_encoder` setting
 * for that sensor, layer and direction, or else from the instance's devicetree. The step counting
 * is adapted from ZMK's behavior_sensor_rotate_common.c (MIT).
 */

#define DT_DRV_COMPAT dax_behavior_encoder

#include <stdlib.h>
#include <string.h>

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <dax/encoder.h>
#include <dax/settings.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(ZMK_KEYMAP_HAS_SENSORS, "dax,behavior-encoder needs a zmk,keymap-sensors node");
BUILD_ASSERT(DT_CHILD_NUM(DT_INST(0, zmk_keymap)) == DT_CHILD_NUM_STATUS_OKAY(DT_INST(0, zmk_keymap)),
             "Remove a layer instead of disabling it: without layer reordering ZMK_KEYMAP_LAYERS_LEN "
             "counts enabled layers only, while ZMK's zmk_sensor_keymap has a row for every one");

struct encoder_config {
    struct zmk_behavior_binding cw;
    struct zmk_behavior_binding ccw;
    uint16_t tap_ms;
};

/* Shared by every instance: a sensor on a layer plays through the one instance bound there. */
static struct {
    struct sensor_value remainder[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
    int triggers[ZMK_KEYMAP_SENSORS_LEN][ZMK_KEYMAP_LAYERS_LEN];
} steps;

/* The instance each layer binds to each sensor, by layer id, built the way ZMK builds
 * zmk_sensor_keymap; NULL where a layer binds something else. */
#define LAYER_SENSOR(idx, layer)                                                                   \
    COND_CODE_1(DT_NODE_HAS_COMPAT(DT_PHANDLE_BY_IDX(layer, sensor_bindings, idx),                 \
                                   dax_behavior_encoder),                                          \
                (DEVICE_DT_GET(DT_PHANDLE_BY_IDX(layer, sensor_bindings, idx))), (NULL))
#define LAYER_ROW(layer)                                                                           \
    COND_CODE_1(DT_NODE_HAS_PROP(layer, sensor_bindings),                                          \
                ({LISTIFY(DT_PROP_LEN(layer, sensor_bindings), LAYER_SENSOR, (, ), layer)}),       \
                ({}))

static const struct device *const instances[ZMK_KEYMAP_LAYERS_LEN][ZMK_KEYMAP_SENSORS_LEN] = {
    DT_FOREACH_CHILD_SEP(DT_INST(0, zmk_keymap), LAYER_ROW, (, ))};

static const struct device *instance_of(uint16_t slot) {
    uint16_t place = slot / 2;
    return instances[place % ZMK_KEYMAP_LAYERS_LEN][place / ZMK_KEYMAP_LAYERS_LEN];
}

static const struct zmk_behavior_binding *devicetree_binding(const struct encoder_config *cfg,
                                                             enum dax_encoder_direction dir) {
    return dir == DAX_ENCODER_CW ? &cfg->cw : &cfg->ccw;
}

static void put_record(uint16_t local_id, uint32_t param1, uint32_t param2, uint16_t tap_ms,
                       uint8_t *buf) {
    sys_put_le16(local_id, &buf[0]);
    sys_put_le32(param1, &buf[2]);
    sys_put_le32(param2, &buf[6]);
    sys_put_le16(tap_ms, &buf[10]);
}

static int binding_default(uint16_t slot, uint8_t *buf, size_t size) {
    const struct device *instance = instance_of(slot);
    if (!instance) {
        return -ENOENT;
    }
    if (size < DAX_ENCODER_RECORD_SIZE) {
        return -ENOSPC;
    }
    const struct encoder_config *cfg = instance->config;
    const struct zmk_behavior_binding *binding = devicetree_binding(cfg, slot % 2);
    put_record(zmk_behavior_get_local_id(binding->behavior_dev), binding->param1, binding->param2,
               cfg->tap_ms, buf);
    return DAX_ENCODER_RECORD_SIZE;
}

/* A binding a step can play: one that presses, on a layer whose sensor plays through this
 * behavior. Not zmk_behavior_validate_binding, which refuses &msc and &mmv: they carry no
 * parameter metadata. */
static int binding_validate(uint16_t slot, const uint8_t *data, size_t len) {
    if (len != DAX_ENCODER_RECORD_SIZE || !instance_of(slot) ||
        sys_get_le16(&data[10]) > DAX_ENCODER_MAX_TAP_MS) {
        return -EINVAL;
    }
    const char *name = zmk_behavior_find_behavior_name_from_local_id(sys_get_le16(&data[0]));
    const struct device *behavior = name ? zmk_behavior_get_binding(name) : NULL;
    if (!behavior) {
        return -EINVAL;
    }
    const struct behavior_driver_api *api = behavior->api;
    return api->binding_pressed ? 0 : -EINVAL;
}

#define SENSOR_NAME(idx, _) DT_NODE_FULL_NAME(ZMK_KEYMAP_SENSORS_BY_IDX(idx))
#define SENSOR_NAME_CHECK(idx, _)                                                                  \
    BUILD_ASSERT(sizeof(SENSOR_NAME(idx, _)) - 1 <= DAX_ENCODER_NAME_SIZE,                         \
                 "A sensor's node name must fit DAX_ENCODER_NAME_SIZE");
LISTIFY(ZMK_KEYMAP_SENSORS_LEN, SENSOR_NAME_CHECK, ())
static const char *const sensor_names[] = {LISTIFY(ZMK_KEYMAP_SENSORS_LEN, SENSOR_NAME, (, ))};

static int sensor_name_default(uint16_t slot, uint8_t *buf, size_t size) {
    size_t len = strlen(sensor_names[slot]);
    if (len > size) {
        return -ENOSPC;
    }
    memcpy(buf, sensor_names[slot], len);
    return len;
}

static const struct dax_setting_type sensor_name_type = {
    .kind = DAX_SETTING_BYTES, .read_only = true, .default_value = sensor_name_default};
static const struct dax_setting_type binding_type = {
    .kind = DAX_SETTING_BYTES, .default_value = binding_default, .validate = binding_validate};

DAX_SETTING_DEFINE_TYPED_SLOTS(dax_encoder_sensor_name, DAX_ENCODER_SUBSYSTEM_ID, "sensor_name",
                               ZMK_KEYMAP_SENSORS_LEN, DAX_ENCODER_NAME_SIZE, &sensor_name_type);
DAX_SETTING_DEFINE_TYPED_SLOTS(dax_encoder_binding, DAX_ENCODER_SUBSYSTEM_ID, "binding",
                               DAX_ENCODER_SLOT_COUNT, DAX_ENCODER_RECORD_SIZE, &binding_type);

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>

static struct zmk_rpc_custom_subsystem_meta encoder_settings_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

// Registered only so a client can tell that the keyboard has these settings; they are read and
// written through the dax_settings subsystem.
static bool encoder_settings_handle_request(const zmk_custom_CallRequest *req,
                                            pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);
    return false;
}

ZMK_RPC_CUSTOM_SUBSYSTEM(dax_encoder, &encoder_settings_meta, encoder_settings_handle_request);
#endif

/* The record written to the slot, if there is one this build can play. An unwritten slot plays
 * the devicetree binding by name: a local id read before the behavior settings load is 0. */
static bool stored_binding(uint16_t slot, struct zmk_behavior_binding *binding, uint16_t *tap_ms) {
    if (!dax_setting_is_set(&dax_encoder_binding, slot)) {
        return false;
    }
    uint8_t record[DAX_ENCODER_RECORD_SIZE];
    bool unreadable = false;
    int len = dax_setting_read(&dax_encoder_binding, slot, record, sizeof(record), &unreadable);
    if (len != DAX_ENCODER_RECORD_SIZE || unreadable) {
        return false;
    }
    const char *name = zmk_behavior_find_behavior_name_from_local_id(sys_get_le16(&record[0]));
    if (!name) {
        return false;
    }
    *binding = (struct zmk_behavior_binding){
        .behavior_dev = name,
        .param1 = sys_get_le32(&record[2]),
        .param2 = sys_get_le32(&record[6]),
    };
    *tap_ms = sys_get_le16(&record[10]);
    return true;
}

static int encoder_accept_data(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event,
                               const struct zmk_sensor_config *sensor_config,
                               size_t channel_data_size,
                               const struct zmk_sensor_channel_data *channel_data) {
    ARG_UNUSED(binding);
    ARG_UNUSED(channel_data_size);
    const struct sensor_value value = channel_data[0].value;
    int sensor = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);
    int triggers;

    /* Sensors that report steps in val2 only, as old EC11 drivers did. */
    if (value.val1 == 0) {
        triggers = value.val2;
    } else {
        struct sensor_value remainder = steps.remainder[sensor][event.layer];
        remainder.val1 += value.val1;
        remainder.val2 += value.val2;
        if (abs(remainder.val2) >= 1000000) {
            remainder.val1 += remainder.val2 / 1000000;
            remainder.val2 %= 1000000;
        }
        int trigger_degrees = 360 / sensor_config->triggers_per_rotation;
        triggers = remainder.val1 / trigger_degrees;
        remainder.val1 %= trigger_degrees;
        steps.remainder[sensor][event.layer] = remainder;
    }
    steps.triggers[sensor][event.layer] = triggers;
    return 0;
}

static int encoder_process(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event,
                           enum behavior_sensor_binding_process_mode mode) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct encoder_config *cfg = dev->config;
    int sensor = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);
    int triggers = steps.triggers[sensor][event.layer];

    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER || triggers == 0) {
        steps.triggers[sensor][event.layer] = 0;
        return ZMK_BEHAVIOR_TRANSPARENT;
    }

    enum dax_encoder_direction dir = triggers > 0 ? DAX_ENCODER_CW : DAX_ENCODER_CCW;
    struct zmk_behavior_binding played;
    uint16_t tap_ms;
    if (!stored_binding(DAX_ENCODER_SLOT(sensor, event.layer, dir), &played, &tap_ms)) {
        played = *devicetree_binding(cfg, dir);
        tap_ms = cfg->tap_ms;
    }

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(trans))
    if (zmk_behavior_get_binding(played.behavior_dev) == DEVICE_DT_GET(DT_NODELABEL(trans))) {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    /* A peripheral's sensor arrives here as its event; the bindings play on this half. */
    event.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif

    for (int i = 0; i < abs(triggers); i++) {
        zmk_behavior_queue_add(&event, played, true, tap_ms);
        zmk_behavior_queue_add(&event, played, false, 0);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api encoder_driver_api = {
    .sensor_binding_accept_data = encoder_accept_data,
    .sensor_binding_process = encoder_process,
};

#define DEVICETREE_BINDING(n, prop)                                                                \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, prop, 0)),                        \
        .param1 = COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(DT_DRV_INST(n), prop, 0, param1),             \
                              (DT_PHA_BY_IDX(DT_DRV_INST(n), prop, 0, param1)), (0)),              \
        .param2 = COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(DT_DRV_INST(n), prop, 0, param2),             \
                              (DT_PHA_BY_IDX(DT_DRV_INST(n), prop, 0, param2)), (0)),              \
    }

#define ENCODER_INST(n)                                                                            \
    BUILD_ASSERT(DT_INST_PROP(n, tap_ms) >= 0 && DT_INST_PROP(n, tap_ms) <= DAX_ENCODER_MAX_TAP_MS, \
                 "tap-ms is out of range");                                                        \
    static const struct encoder_config encoder_config_##n = {                                      \
        .cw = DEVICETREE_BINDING(n, cw_binding),                                                   \
        .ccw = DEVICETREE_BINDING(n, ccw_binding),                                                 \
        .tap_ms = DT_INST_PROP(n, tap_ms),                                                         \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, &encoder_config_##n, POST_KERNEL,                 \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &encoder_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ENCODER_INST)
