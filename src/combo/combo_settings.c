/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 *
 * Adapted from cormoran/zmk-feature-runtime-combo (src/runtime_combo/runtime_combo.c at d9490e7):
 * the storage, defaults and read/write half, under the dax_combo names. The matching half is
 * combo_engine.c, which is ZMK's own combo.c.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <dax/combo.h>
#include <cormoran/zmk/custom_settings.h>

#include <drivers/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* The behavior binding is stored separately, as a parallel BEHAVIOR-typed
 * custom-settings array (see DAX_COMBO_BEHAVIORS_KEY), so the packed
 * body below only carries the rest of a combo's configuration. */
#define DAX_COMBO_STORAGE_VERSION 4
#define DAX_COMBO_FLAG_ENABLED BIT(0)
#define DAX_COMBO_FLAG_SLOW_RELEASE_OVERRIDE BIT(1)
#define DAX_COMBO_FLAG_SLOW_RELEASE_VALUE BIT(2)
#define DAX_COMBO_PACKED_HEADER_SIZE 12
#define DAX_COMBO_PACKED_MAX_SIZE                                                                  \
    (DAX_COMBO_PACKED_HEADER_SIZE + (CONFIG_ZMK_DAX_COMBO_MAX_KEYS_PER_COMBO * sizeof(uint16_t)))

BUILD_ASSERT(DAX_COMBO_PACKED_MAX_SIZE <= CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE,
             "Runtime combo packed value must fit in one custom setting bytes value");

#define DAX_COMBO_EMPTY_BYTES                                                                      \
    ((struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, .size = 0})

/* ZMK_CUSTOM_SETTING_ARRAY_DEFINE seeds every element slot (all max_size, not
 * just default_size) from defaults[index] at init, so each array needs a
 * full-length defaults table. Bare aggregate initializers (not the
 * ZMK_CUSTOM_SETTING_VALUE_* compound-literal macros) keep these valid
 * constant initializers under strict -std=c11, matching the reason
 * ZMK_CUSTOM_SETTING_ARRAY_DEFAULT_INT32_DEFINE avoids the macros. Every
 * default is "empty"; combined with default_size 0 the arrays start empty and
 * grow on write, so an un-grown element reads back -ENOENT and the read paths
 * fall back to any compile-time default. */
#define DAX_COMBO_COMBOS_DEFAULT(i, _) {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, .size = 0}
#define DAX_COMBO_NAMES_DEFAULT(i, _)                                                              \
    {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING, .size = 0, .string_value = ""}
#define DAX_COMBO_BEHAVIORS_DEFAULT(i, _)                                                          \
    {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,                                               \
     .behavior_value = {.behavior_id = 0, .param1 = 0, .param2 = 0}}

static const struct zmk_custom_setting_value
    dax_combo_combos_defaults[CONFIG_ZMK_DAX_COMBO_MAX_COMBOS] = {
        LISTIFY(CONFIG_ZMK_DAX_COMBO_MAX_COMBOS, DAX_COMBO_COMBOS_DEFAULT, (, ))};
static const struct zmk_custom_setting_value
    dax_combo_names_defaults[CONFIG_ZMK_DAX_COMBO_MAX_COMBOS] = {
        LISTIFY(CONFIG_ZMK_DAX_COMBO_MAX_COMBOS, DAX_COMBO_NAMES_DEFAULT, (, ))};
static const struct zmk_custom_setting_value
    dax_combo_behaviors_defaults[CONFIG_ZMK_DAX_COMBO_MAX_COMBOS] = {
        LISTIFY(CONFIG_ZMK_DAX_COMBO_MAX_COMBOS, DAX_COMBO_BEHAVIORS_DEFAULT, (, ))};

/* One registration per array (custom-settings P3 API): each array owns a
 * single contiguous backing buffer sized by max_count, instead of N separate
 * per-element registrations. Element access still happens by base key + index
 * via zmk_custom_setting_*_array_* below. The combo body and name arrays are
 * joined by a parallel BEHAVIOR-typed array so each combo's behavior binding is
 * stored and validated as a first-class binding rather than opaque bytes. */
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(dax_combo_combos, DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_COMBOS_KEY,
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                CONFIG_ZMK_DAX_COMBO_MAX_COMBOS, 0, dax_combo_combos_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_ARRAY_DEFINE(dax_combo_names, DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_NAMES_KEY,
                                ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
                                CONFIG_ZMK_DAX_COMBO_MAX_COMBOS, 0, dax_combo_names_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_ARRAY_DEFINE(dax_combo_behaviors, DAX_COMBO_SUBSYSTEM_ID,
                                DAX_COMBO_BEHAVIORS_KEY, ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR,
                                CONFIG_ZMK_DAX_COMBO_MAX_COMBOS, 0, dax_combo_behaviors_defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

/*
 * Global (non per-combo) tuning settings. Registered through the public
 * ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS macro: since the custom-settings
 * simplification (P4) the descriptor is a const, flash-resident object with a
 * separate RAM state block, so it can no longer be hand-built as a
 * STRUCT_SECTION_ITERABLE with an inline `.temp_slot`/`.default_value`.
 */
ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dax_combo_timeout_ms, DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_TIMEOUT_MS_KEY,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_DAX_COMBO_DEFAULT_TIMEOUT_MS),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(1, 65535));

ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dax_combo_slow_release, DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_SLOW_RELEASE_KEY,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, ZMK_CUSTOM_SETTING_VALUE_BOOL(false),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);

ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dax_combo_require_prior_idle_ms, DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_REQUIRE_PRIOR_IDLE_MS_KEY,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_DAX_COMBO_DEFAULT_REQUIRE_PRIOR_IDLE_MS),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(0, 65535));

/* Compile-time defaults from an optional `dax,runtime-combo-defaults` node. */
struct dax_combo_default {
    uint32_t slot;
    const char *name;
    struct dax_combo_config config;
};

#define DT_DRV_COMPAT dax_runtime_combo_defaults

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define DAX_COMBO_DEFAULT_PROP_BIT_AT_IDX(n, prop, idx) BIT(DT_PROP_BY_IDX(n, prop, idx))
#define DAX_COMBO_DEFAULT_LAYER_MASK(n)                                                            \
    COND_CODE_1(DT_NODE_HAS_PROP(n, layers),                                                       \
                (DT_FOREACH_PROP_ELEM_SEP(n, layers, DAX_COMBO_DEFAULT_PROP_BIT_AT_IDX, (|))),     \
                (0))

#define DAX_COMBO_DEFAULT_SLOW_RELEASE(n)                                                          \
    (DT_PROP(n, slow_release) ? DAX_COMBO_SLOW_RELEASE_ON : DAX_COMBO_SLOW_RELEASE_INHERIT)

#define DAX_COMBO_DEFAULT_VALIDATE(n)                                                              \
    BUILD_ASSERT(DT_PROP_LEN(n, key_positions) >= 1,                                               \
                 "runtime combo default must have at least one key position");                     \
    BUILD_ASSERT(DT_PROP_LEN(n, key_positions) <= CONFIG_ZMK_DAX_COMBO_MAX_KEYS_PER_COMBO,         \
                 "runtime combo default has too many key positions");                              \
    BUILD_ASSERT(DT_PROP_OR(n, slot, DT_NODE_CHILD_IDX(n)) < CONFIG_ZMK_DAX_COMBO_MAX_COMBOS,      \
                 "runtime combo default slot is out of range");                                    \
    BUILD_ASSERT(DT_PROP_OR(n, timeout_ms, 0) <= UINT16_MAX,                                       \
                 "runtime combo default timeout-ms must fit in 16 bits");                          \
    BUILD_ASSERT(DT_PROP_OR(n, require_prior_idle_ms, 0) <= UINT16_MAX,                            \
                 "runtime combo default require-prior-idle-ms must fit in 16 bits");

DT_INST_FOREACH_CHILD(0, DAX_COMBO_DEFAULT_VALIDATE)

#define DAX_COMBO_DEFAULT_INST(n)                                                                  \
    {                                                                                              \
        .slot = DT_PROP_OR(n, slot, DT_NODE_CHILD_IDX(n)),                                         \
        .name = DT_PROP_OR(n, display_name, DT_NODE_FULL_NAME(n)),                                 \
        .config =                                                                                  \
            {                                                                                      \
                .enabled = true,                                                                   \
                .key_position_len = DT_PROP_LEN(n, key_positions),                                 \
                .layer_mask = DAX_COMBO_DEFAULT_LAYER_MASK(n),                                     \
                .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                                      \
                .timeout_ms = DT_PROP_OR(n, timeout_ms, 0),                                        \
                .require_prior_idle_ms = DT_PROP_OR(n, require_prior_idle_ms, 0),                  \
                .slow_release_override = DAX_COMBO_DEFAULT_SLOW_RELEASE(n),                        \
                .key_positions = DT_PROP(n, key_positions),                                        \
            },                                                                                     \
    },

static const struct dax_combo_default dax_combo_defaults[] = {
    DT_INST_FOREACH_CHILD(0, DAX_COMBO_DEFAULT_INST)};

#define DAX_COMBO_DEFAULT_COUNT ARRAY_SIZE(dax_combo_defaults)

#else

static const struct dax_combo_default dax_combo_defaults[1];

#define DAX_COMBO_DEFAULT_COUNT 0

#endif

static const struct dax_combo_default *dax_combo_find_default(uint32_t index) {
    for (size_t i = 0; i < DAX_COMBO_DEFAULT_COUNT; i++) {
        if (dax_combo_defaults[i].slot == index) {
            return &dax_combo_defaults[i];
        }
    }
    return NULL;
}

static void put_u16(uint8_t *dest, uint16_t value) { sys_put_le16(value, dest); }
static void put_u32(uint8_t *dest, uint32_t value) { sys_put_le32(value, dest); }
static uint16_t get_u16(const uint8_t *src) { return sys_get_le16(src); }
static uint32_t get_u32(const uint8_t *src) { return sys_get_le32(src); }

static const struct zmk_custom_setting *combo_setting(uint32_t index) {
    return zmk_custom_setting_find_array_element(DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_COMBOS_KEY,
                                                 index);
}

static const struct zmk_custom_setting *name_setting(uint32_t index) {
    return zmk_custom_setting_find_array_element(DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_NAMES_KEY,
                                                 index);
}

static const struct zmk_custom_setting *behavior_setting(uint32_t index) {
    return zmk_custom_setting_find_array_element(DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_BEHAVIORS_KEY,
                                                 index);
}

/* Read a combo's behavior binding from its BEHAVIOR-typed array element and
 * resolve the stored local ID back to a behavior device name. */
static int read_combo_behavior(uint32_t index, struct zmk_behavior_binding *behavior) {
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_BEHAVIORS_KEY,
                                                   index, &value);
    if (ret < 0) {
        return ret;
    }
    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BEHAVIOR) {
        return -EINVAL;
    }
    zmk_behavior_local_id_t behavior_id = value.behavior_value.behavior_id;
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(behavior_id);
    if (!behavior_name) {
        return -ENODEV;
    }
    *behavior = (struct zmk_behavior_binding){
        .behavior_dev = behavior_name,
        .param1 = value.behavior_value.param1,
        .param2 = value.behavior_value.param2,
    };
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    behavior->local_id = behavior_id;
#endif
    return 0;
}

/* Persist a combo's behavior binding into its BEHAVIOR-typed array element,
 * growing the array to cover `index` if needed. The custom-settings write path
 * validates the binding against the target behavior's parameter metadata. */
static int write_combo_behavior(uint32_t index, const struct zmk_behavior_binding *behavior,
                                bool persist) {
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(behavior->behavior_dev);
    if (behavior_id == 0 || behavior_id == UINT16_MAX) {
        return -ENODEV;
    }
    const struct zmk_custom_setting *setting = behavior_setting(index);
    if (!setting) {
        return -ENOENT;
    }
    struct zmk_custom_setting_value value =
        ZMK_CUSTOM_SETTING_VALUE_BEHAVIOR(behavior_id, behavior->param1, behavior->param2);
    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    return zmk_custom_setting_write_array_element(setting, &value, array_size,
                                                  persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                                          : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

static uint32_t dax_combo_default_upper_bound(void) {
    uint32_t upper_bound = 0;
    for (size_t i = 0; i < DAX_COMBO_DEFAULT_COUNT; i++) {
        upper_bound = MAX(upper_bound, dax_combo_defaults[i].slot + 1);
    }
    return upper_bound;
}

uint32_t dax_combo_count(void) {
    const struct zmk_custom_setting *setting =
        zmk_custom_setting_find_array(DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_COMBOS_KEY);
    uint32_t array_size = setting ? zmk_custom_setting_array_size(setting) : 0;
    /* Slots covered only by a compile-time default may never have grown the
     * underlying custom-settings array, but must still be matched/listed. */
    return MAX(array_size, dax_combo_default_upper_bound());
}

uint32_t dax_combo_max_count(void) { return CONFIG_ZMK_DAX_COMBO_MAX_COMBOS; }

uint32_t dax_combo_default_count(void) { return DAX_COMBO_DEFAULT_COUNT; }

bool dax_combo_has_default(uint32_t index) { return dax_combo_find_default(index) != NULL; }

int dax_combo_read_default(uint32_t index, struct dax_combo_config *combo) {
    if (!combo) {
        return -EINVAL;
    }
    const struct dax_combo_default *def = dax_combo_find_default(index);
    if (!def) {
        return -ENOENT;
    }
    *combo = def->config;
    return 0;
}

static int packed_to_combo(const struct zmk_custom_setting_value *value,
                           struct dax_combo_config *combo) {
    if (value->type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES) {
        return -EINVAL;
    }
    if (value->size == 0) {
        *combo = (struct dax_combo_config){0};
        return 0;
    }

    uint8_t version = value->bytes_value[0];
    if (version != DAX_COMBO_STORAGE_VERSION) {
        return -EINVAL;
    }
    if (value->size < DAX_COMBO_PACKED_HEADER_SIZE) {
        return -EINVAL;
    }

    uint8_t key_position_len = value->bytes_value[2];
    if (key_position_len < 1 || key_position_len > CONFIG_ZMK_DAX_COMBO_MAX_KEYS_PER_COMBO ||
        value->size != DAX_COMBO_PACKED_HEADER_SIZE + key_position_len * sizeof(uint16_t)) {
        return -EINVAL;
    }

    uint8_t flags = value->bytes_value[1];
    enum dax_combo_slow_release_override slow_release_override = DAX_COMBO_SLOW_RELEASE_INHERIT;
    if (flags & DAX_COMBO_FLAG_SLOW_RELEASE_OVERRIDE) {
        slow_release_override = (flags & DAX_COMBO_FLAG_SLOW_RELEASE_VALUE)
                                    ? DAX_COMBO_SLOW_RELEASE_ON
                                    : DAX_COMBO_SLOW_RELEASE_OFF;
    }

    /* The behavior binding lives in a parallel BEHAVIOR-typed array and is
     * filled in by the caller (see read_combo_behavior). */
    *combo = (struct dax_combo_config){
        .enabled = (flags & DAX_COMBO_FLAG_ENABLED) != 0,
        .key_position_len = key_position_len,
        .layer_mask = get_u32(&value->bytes_value[4]),
        .timeout_ms = get_u16(&value->bytes_value[8]),
        .require_prior_idle_ms = get_u16(&value->bytes_value[10]),
        .slow_release_override = slow_release_override,
    };

    for (uint8_t i = 0; i < key_position_len; i++) {
        combo->key_positions[i] =
            get_u16(&value->bytes_value[DAX_COMBO_PACKED_HEADER_SIZE + i * sizeof(uint16_t)]);
    }
    return 0;
}

static int combo_to_packed(const struct dax_combo_config *combo,
                           struct zmk_custom_setting_value *value) {
    if (combo->key_position_len < 1 ||
        combo->key_position_len > CONFIG_ZMK_DAX_COMBO_MAX_KEYS_PER_COMBO) {
        return -EINVAL;
    }

    for (uint8_t i = 0; i < combo->key_position_len; i++) {
        if (combo->key_positions[i] < 0 || combo->key_positions[i] > UINT16_MAX) {
            return -ERANGE;
        }
    }

    *value = (struct zmk_custom_setting_value){
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
        .size = DAX_COMBO_PACKED_HEADER_SIZE + combo->key_position_len * sizeof(uint16_t),
    };
    uint8_t flags = combo->enabled ? DAX_COMBO_FLAG_ENABLED : 0;
    if (combo->slow_release_override != DAX_COMBO_SLOW_RELEASE_INHERIT) {
        flags |= DAX_COMBO_FLAG_SLOW_RELEASE_OVERRIDE;
        if (combo->slow_release_override == DAX_COMBO_SLOW_RELEASE_ON) {
            flags |= DAX_COMBO_FLAG_SLOW_RELEASE_VALUE;
        }
    }
    value->bytes_value[0] = DAX_COMBO_STORAGE_VERSION;
    value->bytes_value[1] = flags;
    value->bytes_value[2] = combo->key_position_len;
    value->bytes_value[3] = 0;
    put_u32(&value->bytes_value[4], combo->layer_mask);
    put_u16(&value->bytes_value[8], combo->timeout_ms);
    put_u16(&value->bytes_value[10], combo->require_prior_idle_ms);

    for (uint8_t i = 0; i < combo->key_position_len; i++) {
        put_u16(&value->bytes_value[DAX_COMBO_PACKED_HEADER_SIZE + i * sizeof(uint16_t)],
                combo->key_positions[i]);
    }
    return 0;
}

int dax_combo_read(uint32_t index, struct dax_combo_config *combo) {
    if (!combo || index >= CONFIG_ZMK_DAX_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_COMBOS_KEY,
                                                   index, &value);
    if (ret < 0) {
        /* -ENOENT also covers "within array_max_size but the array hasn't
         * grown this far yet", which is indistinguishable from an explicit
         * empty value as far as the default fallback is concerned. */
        if (ret == -ENOENT) {
            const struct dax_combo_default *def = dax_combo_find_default(index);
            if (def) {
                *combo = def->config;
                return 0;
            }
        }
        return ret;
    }
    if (value.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES && value.size == 0) {
        const struct dax_combo_default *def = dax_combo_find_default(index);
        if (def) {
            *combo = def->config;
            return 0;
        }
    }
    ret = packed_to_combo(&value, combo);
    if (ret < 0 || combo->key_position_len == 0) {
        /* An empty (size 0) record with no default decodes to a zeroed combo
         * that has no behavior binding to read. */
        return ret;
    }
    return read_combo_behavior(index, &combo->behavior);
}

int dax_combo_read_name(uint32_t index, char *name, size_t name_size) {
    if (!name || name_size == 0 || index >= CONFIG_ZMK_DAX_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_NAMES_KEY,
                                                   index, &value);
    if (ret < 0) {
        if (ret == -ENOENT) {
            const struct dax_combo_default *def = dax_combo_find_default(index);
            if (def) {
                strncpy(name, def->name, name_size - 1);
                name[name_size - 1] = '\0';
                return 0;
            }
        }
        name[0] = '\0';
        return ret;
    }
    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING) {
        return -EINVAL;
    }
    if (value.string_value[0] == '\0') {
        const struct dax_combo_default *def = dax_combo_find_default(index);
        if (def) {
            strncpy(name, def->name, name_size - 1);
            name[name_size - 1] = '\0';
            return 0;
        }
    }
    strncpy(name, value.string_value, name_size - 1);
    name[name_size - 1] = '\0';
    return 0;
}

int dax_combo_read_global_settings(struct dax_combo_global_settings *settings) {
    if (!settings) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value timeout_value;
    int ret = zmk_custom_setting_read_by_key(DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_TIMEOUT_MS_KEY,
                                             &timeout_value);
    if (ret < 0) {
        return ret;
    }
    if (timeout_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 ||
        timeout_value.int32_value < 1 || timeout_value.int32_value > UINT16_MAX) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value slow_release_value;
    ret = zmk_custom_setting_read_by_key(DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_SLOW_RELEASE_KEY,
                                         &slow_release_value);
    if (ret < 0) {
        return ret;
    }
    if (slow_release_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value require_prior_idle_value;
    ret = zmk_custom_setting_read_by_key(
        DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_REQUIRE_PRIOR_IDLE_MS_KEY, &require_prior_idle_value);
    if (ret < 0) {
        return ret;
    }
    if (require_prior_idle_value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 ||
        require_prior_idle_value.int32_value < 0 ||
        require_prior_idle_value.int32_value > UINT16_MAX) {
        return -EINVAL;
    }

    *settings = (struct dax_combo_global_settings){
        .timeout_ms = timeout_value.int32_value,
        .slow_release = slow_release_value.bool_value,
        .max_combo = dax_combo_max_count(),
        .require_prior_idle_ms = require_prior_idle_value.int32_value,
    };
    return 0;
}

static int ensure_name_array_size(uint32_t index, bool persist) {
    const struct zmk_custom_setting *setting = name_setting(index);
    if (!setting) {
        return -ENOENT;
    }
    if (zmk_custom_setting_array_size(setting) > index) {
        return 0;
    }
    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    return zmk_custom_setting_write_array_element(
        setting, &ZMK_CUSTOM_SETTING_VALUE_STRING(""), array_size,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

int dax_combo_write(uint32_t index, const struct dax_combo_config *combo, bool persist) {
    if (!combo || index >= CONFIG_ZMK_DAX_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    /* Resolve and validate the behavior binding up front so an invalid binding
     * fails before any array element is written or grown. */
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(combo->behavior.behavior_dev);
    if (behavior_id == 0 || behavior_id == UINT16_MAX) {
        return -ENODEV;
    }
    int ret = zmk_behavior_validate_binding(&combo->behavior);
    if (ret < 0) {
        return ret;
    }

    struct zmk_custom_setting_value value;
    ret = combo_to_packed(combo, &value);
    if (ret < 0) {
        return ret;
    }

    const struct zmk_custom_setting *setting = combo_setting(index);
    if (!setting) {
        return -ENOENT;
    }

    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    int write_ret = zmk_custom_setting_write_array_element(
        setting, &value, array_size,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    int behavior_ret = write_combo_behavior(index, &combo->behavior, persist);
    int name_ret = ensure_name_array_size(index, persist);
    if (write_ret < 0) {
        return write_ret;
    }
    if (behavior_ret < 0) {
        return behavior_ret;
    }
    return name_ret;
}

int dax_combo_write_name(uint32_t index, const char *name, bool persist) {
    if (!name || index >= CONFIG_ZMK_DAX_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }
    const struct zmk_custom_setting *setting = name_setting(index);
    if (!setting) {
        return -ENOENT;
    }

    struct zmk_custom_setting_value value = {
        .type = ZMK_CUSTOM_SETTING_VALUE_TYPE_STRING,
    };
    strncpy(value.string_value, name, CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
    value.string_value[CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE] = '\0';
    value.size = strlen(value.string_value);

    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    return zmk_custom_setting_write_array_element(setting, &value, array_size,
                                                  persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                                          : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

int dax_combo_write_timeout_ms(uint16_t timeout_ms, bool persist) {
    if (timeout_ms == 0) {
        return -EINVAL;
    }

    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(timeout_ms);
    int ret = zmk_custom_setting_write_by_key(
        DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_TIMEOUT_MS_KEY, &value,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    return ret;
}

int dax_combo_write_slow_release(bool slow_release, bool persist) {
    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_BOOL(slow_release);
    int ret = zmk_custom_setting_write_by_key(
        DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_SLOW_RELEASE_KEY, &value,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    return ret;
}

int dax_combo_write_require_prior_idle_ms(uint16_t require_prior_idle_ms, bool persist) {
    struct zmk_custom_setting_value value = ZMK_CUSTOM_SETTING_VALUE_INT32(require_prior_idle_ms);
    int ret = zmk_custom_setting_write_by_key(
        DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_REQUIRE_PRIOR_IDLE_MS_KEY, &value,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    return ret;
}

int dax_combo_delete(uint32_t index, bool persist) {
    struct dax_combo_config combo;
    int ret = dax_combo_read(index, &combo);
    if (ret < 0) {
        return ret;
    }
    combo.enabled = false;
    return dax_combo_write(index, &combo, persist);
}

bool dax_combo_has_override(uint32_t index) {
    if (index >= CONFIG_ZMK_DAX_COMBO_MAX_COMBOS) {
        return false;
    }
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(DAX_COMBO_SUBSYSTEM_ID, DAX_COMBO_COMBOS_KEY,
                                                   index, &value);
    return ret == 0 && value.size > 0;
}

int dax_combo_reset(uint32_t index) {
    if (index >= CONFIG_ZMK_DAX_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }

    /* zmk_custom_setting_reset() restores default_array_size, which is
     * shared by the whole array (not per-element): calling it here would
     * shrink every other combo's visible array_size back to 0. Persisting an
     * explicit empty value at just this index is observably identical to an
     * unset slot for dax_combo_read()'s fallback, without that
     * side effect. */
    const struct zmk_custom_setting *setting = combo_setting(index);
    if (!setting) {
        return -ENOENT;
    }
    uint32_t array_size = MAX(zmk_custom_setting_array_size(setting), index + 1);
    int ret = zmk_custom_setting_write_array_element(setting, &DAX_COMBO_EMPTY_BYTES, array_size,
                                                     ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);

    const struct zmk_custom_setting *name = name_setting(index);
    if (name) {
        uint32_t name_array_size = MAX(zmk_custom_setting_array_size(name), index + 1);
        zmk_custom_setting_write_array_element(name, &ZMK_CUSTOM_SETTING_VALUE_STRING(""),
                                               name_array_size,
                                               ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    }
    return ret;
}

static int dax_combo_settings_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *changed = as_zmk_custom_setting_changed(eh);

    /* Every kind counts, SAVED included: a persisting write or a reset raises only SAVED. The
     * table is read from the work queue, because this listener runs inside the write that
     * raised it, before the other arrays of the same combo are written. */
    if (changed == NULL ||
        zmk_custom_setting_matches(changed->setting, DAX_COMBO_SUBSYSTEM_ID, NULL, NULL)) {
        dax_combo_engine_request_rebuild();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_combo_settings, dax_combo_settings_listener);
ZMK_SUBSCRIPTION(dax_combo_settings, zmk_custom_setting_changed);
ZMK_SUBSCRIPTION(dax_combo_settings, zmk_custom_settings_initialized);
