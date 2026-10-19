/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 *
 * Adapted from cormoran/zmk-feature-runtime-macro (src/runtime_macro.c and
 * src/studio/runtime_macro_handler.c at 3853096): the body codec, under the dax_macro names.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/behavior.h>

#include "macro_body.h"

#define PACKED_KEY_LSFT BIT(7)
#define PACKED_KEY_USAGE_MASK 0x7f
#define PACKED_KEY_MIN_USAGE 0x04
#define PACKED_KEY_MAX_USAGE 0x38

int dax_macro_pack_key(uint32_t keycode, uint8_t *packed) {
    uint32_t mods = SELECT_MODS(keycode);
    if ((mods & ~MOD_LSFT) != 0) {
        return -EINVAL;
    }
    uint32_t usage_keycode = STRIP_MODS(keycode);
    if (ZMK_HID_USAGE_PAGE(usage_keycode) != HID_USAGE_KEY) {
        return -EINVAL;
    }
    uint32_t usage = ZMK_HID_USAGE_ID(usage_keycode);
    if (usage < PACKED_KEY_MIN_USAGE || usage > PACKED_KEY_MAX_USAGE) {
        return -EINVAL;
    }
    *packed = usage | (mods == MOD_LSFT ? PACKED_KEY_LSFT : 0);
    return 0;
}

static bool packed_key_valid(uint8_t packed) {
    uint8_t usage = packed & PACKED_KEY_USAGE_MASK;
    return usage >= PACKED_KEY_MIN_USAGE && usage <= PACKED_KEY_MAX_USAGE;
}

static int read_uvar(const uint8_t *body, size_t len, size_t *offset, uint32_t *value) {
    uint32_t result = 0;
    for (uint8_t shift = 0; *offset < len && shift <= 28; shift += 7) {
        uint8_t byte = body[(*offset)++];
        /* The fifth byte has room for four bits of a uint32. */
        if (shift == 28 && (byte & 0xf0) != 0) {
            return -EINVAL;
        }
        result |= (uint32_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return 0;
        }
    }
    return -EINVAL;
}

static int put_byte(uint8_t *body, size_t size, size_t *offset, uint8_t byte) {
    if (*offset >= size) {
        return -EMSGSIZE;
    }
    body[(*offset)++] = byte;
    return 0;
}

static int put_uvar(uint8_t *body, size_t size, size_t *offset, uint32_t value) {
    do {
        uint8_t byte = value & 0x7f;
        value >>= 7;
        int ret = put_byte(body, size, offset, value != 0 ? (byte | 0x80) : byte);
        if (ret < 0) {
            return ret;
        }
    } while (value != 0);
    return 0;
}

int dax_macro_next_step(const uint8_t *body, size_t len, size_t *offset,
                        struct dax_macro_step *step) {
    if (*offset == 0) {
        if (len == 0 || body[0] != DAX_MACRO_BODY_VERSION) {
            return -EINVAL;
        }
        *offset = 1;
    }
    if (*offset >= len) {
        return -ENOENT;
    }

    *step = (struct dax_macro_step){.kind = body[(*offset)++]};
    uint32_t value;
    int ret;
    switch (step->kind) {
    case DAX_MACRO_STEP_DOWN:
    case DAX_MACRO_STEP_UP:
    case DAX_MACRO_STEP_TAP:
        ret = read_uvar(body, len, offset, &value);
        if (ret < 0 || value > UINT16_MAX) {
            return -EINVAL;
        }
        step->behavior_id = value;
        ret = read_uvar(body, len, offset, &step->param1);
        return ret < 0 ? ret : read_uvar(body, len, offset, &step->param2);
    case DAX_MACRO_STEP_DELAY:
        return read_uvar(body, len, offset, &step->delay_ms);
    case DAX_MACRO_STEP_KEY_TAP_SEQUENCE:
        ret = read_uvar(body, len, offset, &value);
        if (ret < 0 || value > DAX_MACRO_KEY_TAP_SEQUENCE_MAX || value > len - *offset) {
            return -EINVAL;
        }
        for (uint32_t i = 0; i < value; i++) {
            if (!packed_key_valid(body[*offset + i])) {
                return -EINVAL;
            }
        }
        step->key_count = value;
        memcpy(step->keys, &body[*offset], value);
        *offset += value;
        return 0;
    default:
        return -EINVAL;
    }
}

static int check(const uint8_t *body, size_t len, bool bindings) {
    struct dax_macro_step step;
    size_t offset = 0;
    int ret;
    while ((ret = dax_macro_next_step(body, len, &offset, &step)) == 0) {
        if (!bindings) {
            continue;
        }
        struct zmk_behavior_binding binding;
        if (step.kind == DAX_MACRO_STEP_KEY_TAP_SEQUENCE) {
            for (uint8_t i = 0; i < step.key_count && ret == 0; i++) {
                ret = dax_macro_key_binding(step.keys[i], &binding);
            }
        } else if (step.kind != DAX_MACRO_STEP_DELAY) {
            ret = dax_macro_step_binding(&step, &binding);
        }
        if (ret < 0) {
            return ret;
        }
    }
    return ret == -ENOENT ? 0 : ret;
}

int dax_macro_body_check(const uint8_t *body, size_t len) { return check(body, len, false); }

int dax_macro_body_check_bindings(const uint8_t *body, size_t len) {
    return check(body, len, true);
}

int dax_macro_body_put_step(uint8_t *body, size_t size, size_t *offset,
                            const struct dax_macro_step *step) {
    int ret;
    switch (step->kind) {
    case DAX_MACRO_STEP_DOWN:
    case DAX_MACRO_STEP_UP:
    case DAX_MACRO_STEP_TAP:
        ret = put_byte(body, size, offset, step->kind);
        ret = ret ?: put_uvar(body, size, offset, step->behavior_id);
        ret = ret ?: put_uvar(body, size, offset, step->param1);
        return ret ?: put_uvar(body, size, offset, step->param2);
    case DAX_MACRO_STEP_DELAY:
        ret = put_byte(body, size, offset, step->kind);
        return ret ?: put_uvar(body, size, offset, step->delay_ms);
    case DAX_MACRO_STEP_KEY_TAP_SEQUENCE:
        if (step->key_count > DAX_MACRO_KEY_TAP_SEQUENCE_MAX) {
            return -EINVAL;
        }
        for (uint8_t i = 0; i < step->key_count; i++) {
            if (!packed_key_valid(step->keys[i])) {
                return -EINVAL;
            }
        }
        ret = put_byte(body, size, offset, step->kind);
        ret = ret ?: put_uvar(body, size, offset, step->key_count);
        for (uint8_t i = 0; i < step->key_count && ret == 0; i++) {
            ret = put_byte(body, size, offset, step->keys[i]);
        }
        return ret;
    default:
        return -EINVAL;
    }
}

static int resolve(zmk_behavior_local_id_t id, uint32_t param1, uint32_t param2,
                   struct zmk_behavior_binding *binding) {
    const char *name = zmk_behavior_find_behavior_name_from_local_id(id);
    if (!name) {
        return -ENODEV;
    }
    *binding = (struct zmk_behavior_binding){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        .local_id = id,
#endif
        .behavior_dev = name,
        .param1 = param1,
        .param2 = param2,
    };
    return zmk_behavior_validate_binding(binding);
}

int dax_macro_step_binding(const struct dax_macro_step *step,
                           struct zmk_behavior_binding *binding) {
    return resolve(step->behavior_id, step->param1, step->param2, binding);
}

int dax_macro_key_binding(uint8_t packed, struct zmk_behavior_binding *binding) {
    if (!packed_key_valid(packed)) {
        return -EINVAL;
    }
    uint32_t keycode = ZMK_HID_USAGE(HID_USAGE_KEY, (packed & PACKED_KEY_USAGE_MASK));
    if (packed & PACKED_KEY_LSFT) {
        keycode = LS(keycode);
    }
    zmk_behavior_local_id_t kp = zmk_behavior_get_local_id(DEVICE_DT_NAME(DT_NODELABEL(kp)));
    return kp == UINT16_MAX ? -ENODEV : resolve(kp, keycode, 0, binding);
}
