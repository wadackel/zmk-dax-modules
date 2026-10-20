/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * The pointer processors' values as typed dax settings of the `dax_pointer` owner, one slot per
 * processor. The devicetree values are the defaults, and the keys and their forms are listed in
 * proto/dax/settings/settings.proto.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <dax/pointer.h>
#include <dax/settings.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>

#define NAME_SIZE 24

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>

static struct zmk_rpc_custom_subsystem_meta pointer_settings_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

// Registered only so a client can tell that the keyboard has these settings; they are read and
// written through the dax_settings subsystem.
static bool pointer_settings_handle_request(const zmk_custom_CallRequest *req,
                                            pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);
    return false;
}

ZMK_RPC_CUSTOM_SUBSYSTEM(dax_pointer, &pointer_settings_meta, pointer_settings_handle_request);
#endif

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

#define VALUE_DEFAULT(name, put, field)                                                            \
    static int name(uint16_t slot, uint8_t *buf, size_t size) {                                    \
        struct dax_pointer_values defaults;                                                        \
        dax_pointer_defaults(slot, &defaults);                                                     \
        return put(defaults.field, buf, size);                                                     \
    }

VALUE_DEFAULT(scale_mul_default, put_int32, scale_mul)
VALUE_DEFAULT(scale_div_default, put_int32, scale_div)
VALUE_DEFAULT(rotation_default, put_int32, rotation)
VALUE_DEFAULT(xy_swap_default, put_bool, xy_swap)
VALUE_DEFAULT(x_invert_default, put_bool, x_invert)
VALUE_DEFAULT(y_invert_default, put_bool, y_invert)
VALUE_DEFAULT(xy_to_scroll_default, put_bool, xy_to_scroll)
VALUE_DEFAULT(layers_default, put_int32, layers)
VALUE_DEFAULT(temp_layer_on_default, put_bool, temp_layer_on)
VALUE_DEFAULT(temp_layer_default, put_int32, temp_layer)
VALUE_DEFAULT(temp_layer_after_ms_default, put_int32, temp_layer_after_ms)
VALUE_DEFAULT(temp_layer_back_ms_default, put_int32, temp_layer_back_ms)

static int name_default(uint16_t slot, uint8_t *buf, size_t size) {
    const char *name = dax_pointer_name(slot);
    size_t len = MIN(strlen(name), NAME_SIZE);
    if (len > size) {
        return -ENOSPC;
    }
    memcpy(buf, name, len);
    return len;
}

#define NUMBER_TYPE(name, _min, _max, _default)                                                    \
    static const struct dax_setting_type name = {                                                  \
        .kind = DAX_SETTING_INT32, .min = _min, .max = _max, .default_value = _default}
#define FLAG_TYPE(name, _default)                                                                  \
    static const struct dax_setting_type name = {.kind = DAX_SETTING_BOOL,                        \
                                                 .default_value = _default}

NUMBER_TYPE(scale_mul_type, 1, 65535, scale_mul_default);
/* The input listener keeps a processor's remainder in 16 bits, and it can reach the divisor. */
NUMBER_TYPE(scale_div_type, 1, 32767, scale_div_default);
NUMBER_TYPE(rotation_type, -360, 360, rotation_default);
FLAG_TYPE(xy_swap_type, xy_swap_default);
FLAG_TYPE(x_invert_type, x_invert_default);
FLAG_TYPE(y_invert_type, y_invert_default);
FLAG_TYPE(xy_to_scroll_type, xy_to_scroll_default);
NUMBER_TYPE(layers_type, 0, INT32_MAX, layers_default);
FLAG_TYPE(temp_layer_on_type, temp_layer_on_default);
NUMBER_TYPE(temp_layer_type, 0, ZMK_KEYMAP_LAYERS_LEN - 1, temp_layer_default);
NUMBER_TYPE(temp_layer_after_ms_type, 0, 60000, temp_layer_after_ms_default);
NUMBER_TYPE(temp_layer_back_ms_type, 0, 60000, temp_layer_back_ms_default);
static const struct dax_setting_type name_type = {
    .kind = DAX_SETTING_BYTES, .read_only = true, .default_value = name_default};

#define POINTER_SETTING(name, key, size, type)                                                     \
    DAX_SETTING_DEFINE_TYPED_SLOTS(name, DAX_POINTER_SUBSYSTEM_ID, key, DAX_POINTER_COUNT, size,  \
                                   &type)

POINTER_SETTING(dax_pointer_name_setting, "name", NAME_SIZE, name_type);
POINTER_SETTING(dax_pointer_scale_mul, "scale_mul", sizeof(int32_t), scale_mul_type);
POINTER_SETTING(dax_pointer_scale_div, "scale_div", sizeof(int32_t), scale_div_type);
POINTER_SETTING(dax_pointer_rotation, "rotation", sizeof(int32_t), rotation_type);
POINTER_SETTING(dax_pointer_xy_swap, "xy_swap", 1, xy_swap_type);
POINTER_SETTING(dax_pointer_x_invert, "x_invert", 1, x_invert_type);
POINTER_SETTING(dax_pointer_y_invert, "y_invert", 1, y_invert_type);
POINTER_SETTING(dax_pointer_xy_to_scroll, "xy_to_scroll", 1, xy_to_scroll_type);
POINTER_SETTING(dax_pointer_layers, "layers", sizeof(int32_t), layers_type);
POINTER_SETTING(dax_pointer_temp_layer_on, "temp_layer_on", 1, temp_layer_on_type);
POINTER_SETTING(dax_pointer_temp_layer, "temp_layer", sizeof(int32_t), temp_layer_type);
POINTER_SETTING(dax_pointer_temp_layer_after_ms, "temp_layer_after_ms", sizeof(int32_t),
                temp_layer_after_ms_type);
POINTER_SETTING(dax_pointer_temp_layer_back_ms, "temp_layer_back_ms", sizeof(int32_t),
                temp_layer_back_ms_type);

/* An unset slot, or one whose stored value its type refuses, reads as its default. */
static void read_slot(uint16_t slot, struct dax_pointer_values *values) {
    struct dax_pointer_values d;
    dax_pointer_defaults(slot, &d);
    *values = (struct dax_pointer_values){
        .scale_mul = dax_setting_read_int32(&dax_pointer_scale_mul, slot, d.scale_mul),
        .scale_div = dax_setting_read_int32(&dax_pointer_scale_div, slot, d.scale_div),
        .rotation = dax_setting_read_int32(&dax_pointer_rotation, slot, d.rotation),
        .xy_swap = dax_setting_read_bool(&dax_pointer_xy_swap, slot, d.xy_swap),
        .x_invert = dax_setting_read_bool(&dax_pointer_x_invert, slot, d.x_invert),
        .y_invert = dax_setting_read_bool(&dax_pointer_y_invert, slot, d.y_invert),
        .xy_to_scroll = dax_setting_read_bool(&dax_pointer_xy_to_scroll, slot, d.xy_to_scroll),
        .layers = dax_setting_read_int32(&dax_pointer_layers, slot, d.layers),
        .temp_layer_on = dax_setting_read_bool(&dax_pointer_temp_layer_on, slot, d.temp_layer_on),
        .temp_layer = dax_setting_read_int32(&dax_pointer_temp_layer, slot, d.temp_layer),
        .temp_layer_after_ms =
            dax_setting_read_int32(&dax_pointer_temp_layer_after_ms, slot, d.temp_layer_after_ms),
        .temp_layer_back_ms =
            dax_setting_read_int32(&dax_pointer_temp_layer_back_ms, slot, d.temp_layer_back_ms),
    };
}

static void refresh_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    for (uint16_t slot = 0; slot < DAX_POINTER_COUNT; slot++) {
        struct dax_pointer_values values;
        read_slot(slot, &values);
        dax_pointer_apply(slot, &values);
    }
}

static K_WORK_DEFINE(refresh_work, refresh_work_handler);

static int pointer_settings_listener(const zmk_event_t *eh) {
    const struct dax_setting_changed *changed = as_dax_setting_changed(eh);

    /* Settings are read from the work queue: this listener runs inside the write that raised it. */
    if (changed == NULL || strcmp(changed->owner, DAX_POINTER_SUBSYSTEM_ID) == 0) {
        k_work_submit(&refresh_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_pointer_settings, pointer_settings_listener);
ZMK_SUBSCRIPTION(dax_pointer_settings, dax_setting_changed);
ZMK_SUBSCRIPTION(dax_pointer_settings, dax_settings_loaded);
