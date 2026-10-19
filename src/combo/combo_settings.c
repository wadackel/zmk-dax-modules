/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 *
 * Adapted from cormoran/zmk-feature-runtime-combo (src/runtime_combo/runtime_combo.c at d9490e7):
 * the defaults and the read/write half, under the dax_combo names, stored in the dax settings
 * layer. The matching half is combo_engine.c, which is ZMK's own combo.c.
 *
 * One record per slot holds everything about a combo, so a slot is never half in flash:
 *
 *   0      version (1)
 *   1      flags: bit 0 enabled, bit 1 slow release overridden, bit 2 its value
 *   2      key positions n (1..CONFIG_ZMK_DAX_COMBO_MAX_KEYS_PER_COMBO)
 *   3      name length m (0..DAX_COMBO_NAME_MAX)
 *   4..7   layer mask, bit n the nth layer in keymap order (0 = every layer)
 *   8..9   timeout in ms (0 = the global one)
 *   10..11 require-prior-idle in ms (0 = the global one)
 *   12..13 behavior local id
 *   14..17 param1
 *   18..21 param2
 *   22..   n key positions (u16 each), then m name bytes
 *
 * Little-endian throughout. The local id is resolved when the combo is read, not when settings
 * load: ZMK's table of local ids is itself read from settings and may not be there yet.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <dax/combo.h>
#include <dax/settings.h>

#include <drivers/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RECORD_VERSION 1
#define FLAG_ENABLED BIT(0)
#define FLAG_SLOW_RELEASE_OVERRIDE BIT(1)
#define FLAG_SLOW_RELEASE_VALUE BIT(2)
#define RECORD_HEADER_SIZE 22
#define RECORD_MAX_SIZE                                                                            \
    (RECORD_HEADER_SIZE + CONFIG_ZMK_DAX_COMBO_MAX_KEYS_PER_COMBO * sizeof(uint16_t) +            \
     DAX_COMBO_NAME_MAX)

DAX_SETTING_DEFINE(dax_combo_slots, DAX_COMBO_SUBSYSTEM_ID, "combo", CONFIG_ZMK_DAX_COMBO_MAX_COMBOS,
                   RECORD_MAX_SIZE);
DAX_SETTING_DEFINE(dax_combo_timeout, DAX_COMBO_SUBSYSTEM_ID, "timeout_ms", 1, sizeof(uint16_t));
DAX_SETTING_DEFINE(dax_combo_slow_release, DAX_COMBO_SUBSYSTEM_ID, "slow_release", 1, 1);
DAX_SETTING_DEFINE(dax_combo_prior_idle, DAX_COMBO_SUBSYSTEM_ID, "require_prior_idle_ms", 1,
                   sizeof(uint16_t));

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

static const struct dax_combo_default *find_default(uint32_t index) {
    for (size_t i = 0; i < DAX_COMBO_DEFAULT_COUNT; i++) {
        if (dax_combo_defaults[i].slot == index) {
            return &dax_combo_defaults[i];
        }
    }
    return NULL;
}

/* A combo and its name, as one slot holds them. */
struct record {
    struct dax_combo_config config;
    char name[DAX_COMBO_NAME_MAX + 1];
};

static void resolve_behavior(struct dax_combo_config *config) {
    config->behavior.behavior_dev = zmk_behavior_find_behavior_name_from_local_id(config->behavior_id);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    config->behavior.local_id = config->behavior_id;
#endif
}

static size_t encode(const struct record *record, uint8_t *out) {
    const struct dax_combo_config *c = &record->config;
    size_t name_len = strlen(record->name);
    uint8_t flags = c->enabled ? FLAG_ENABLED : 0;
    if (c->slow_release_override != DAX_COMBO_SLOW_RELEASE_INHERIT) {
        flags |= FLAG_SLOW_RELEASE_OVERRIDE;
        if (c->slow_release_override == DAX_COMBO_SLOW_RELEASE_ON) {
            flags |= FLAG_SLOW_RELEASE_VALUE;
        }
    }
    out[0] = RECORD_VERSION;
    out[1] = flags;
    out[2] = c->key_position_len;
    out[3] = name_len;
    sys_put_le32(c->layer_mask, &out[4]);
    sys_put_le16(c->timeout_ms, &out[8]);
    sys_put_le16(c->require_prior_idle_ms, &out[10]);
    sys_put_le16(c->behavior_id, &out[12]);
    sys_put_le32(c->behavior.param1, &out[14]);
    sys_put_le32(c->behavior.param2, &out[18]);
    uint8_t *keys = &out[RECORD_HEADER_SIZE];
    for (uint8_t i = 0; i < c->key_position_len; i++) {
        sys_put_le16(c->key_positions[i], &keys[i * sizeof(uint16_t)]);
    }
    memcpy(&keys[c->key_position_len * sizeof(uint16_t)], record->name, name_len);
    return RECORD_HEADER_SIZE + c->key_position_len * sizeof(uint16_t) + name_len;
}

static int decode(const uint8_t *in, size_t len, struct record *record) {
    if (len < RECORD_HEADER_SIZE || in[0] != RECORD_VERSION) {
        return -EBADMSG;
    }
    uint8_t keys = in[2];
    uint8_t name_len = in[3];
    if (keys < 1 || keys > CONFIG_ZMK_DAX_COMBO_MAX_KEYS_PER_COMBO || name_len > DAX_COMBO_NAME_MAX ||
        len != RECORD_HEADER_SIZE + keys * sizeof(uint16_t) + name_len) {
        return -EBADMSG;
    }
    uint8_t flags = in[1];
    *record = (struct record){
        .config =
            {
                .enabled = (flags & FLAG_ENABLED) != 0,
                .key_position_len = keys,
                .layer_mask = sys_get_le32(&in[4]),
                .timeout_ms = sys_get_le16(&in[8]),
                .require_prior_idle_ms = sys_get_le16(&in[10]),
                .behavior_id = sys_get_le16(&in[12]),
                .behavior = {.param1 = sys_get_le32(&in[14]), .param2 = sys_get_le32(&in[18])},
                .slow_release_override =
                    !(flags & FLAG_SLOW_RELEASE_OVERRIDE) ? DAX_COMBO_SLOW_RELEASE_INHERIT
                    : (flags & FLAG_SLOW_RELEASE_VALUE)   ? DAX_COMBO_SLOW_RELEASE_ON
                                                          : DAX_COMBO_SLOW_RELEASE_OFF,
            },
    };
    const uint8_t *positions = &in[RECORD_HEADER_SIZE];
    for (uint8_t i = 0; i < keys; i++) {
        record->config.key_positions[i] = sys_get_le16(&positions[i * sizeof(uint16_t)]);
    }
    memcpy(record->name, &positions[keys * sizeof(uint16_t)], name_len);
    record->name[name_len] = '\0';
    resolve_behavior(&record->config);
    return 0;
}

/* What the slot holds: its record, or its default with an empty name (shown as the default's). */
static int read_record(uint32_t index, struct record *record) {
    if (index >= CONFIG_ZMK_DAX_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }
    uint8_t buf[RECORD_MAX_SIZE];
    int len = dax_setting_get(&dax_combo_slots, index, buf, sizeof(buf));
    if (len >= 0) {
        int ret = decode(buf, len, record);
        if (ret < 0) {
            LOG_DBG("combo slot %u: stored record unreadable, treated as empty", index);
        }
        return ret;
    }
    if (len != -ENOENT) {
        return len;
    }
    const struct dax_combo_default *def = find_default(index);
    if (!def) {
        return -ENOENT;
    }
    *record = (struct record){.config = def->config};
    record->config.behavior_id = zmk_behavior_get_local_id(def->config.behavior.behavior_dev);
    return 0;
}

static int write_record(uint32_t index, const struct record *record) {
    uint8_t buf[RECORD_MAX_SIZE];
    size_t len = encode(record, buf);
    return dax_setting_set(&dax_combo_slots, index, buf, len);
}

int dax_combo_read(uint32_t index, struct dax_combo_config *combo) {
    if (!combo) {
        return -EINVAL;
    }
    struct record record;
    int ret = read_record(index, &record);
    if (ret == 0) {
        *combo = record.config;
    }
    return ret;
}

int dax_combo_read_name(uint32_t index, char *name, size_t name_size) {
    if (!name || name_size == 0) {
        return -EINVAL;
    }
    name[0] = '\0';
    struct record record;
    int ret = read_record(index, &record);
    if (ret < 0) {
        return ret;
    }
    const struct dax_combo_default *def = find_default(index);
    const char *shown = record.name[0] == '\0' && def ? def->name : record.name;
    strncpy(name, shown, name_size - 1);
    name[name_size - 1] = '\0';
    return 0;
}

static uint16_t read_u16(const struct dax_setting *setting, uint16_t fallback) {
    uint8_t buf[sizeof(uint16_t)];
    return dax_setting_get(setting, 0, buf, sizeof(buf)) == sizeof(buf) ? sys_get_le16(buf)
                                                                        : fallback;
}

int dax_combo_read_global_settings(struct dax_combo_global_settings *settings) {
    if (!settings) {
        return -EINVAL;
    }
    uint8_t slow_release;
    bool slow_release_set = dax_setting_get(&dax_combo_slow_release, 0, &slow_release, 1) == 1;
    uint16_t timeout_ms = read_u16(&dax_combo_timeout, CONFIG_ZMK_DAX_COMBO_DEFAULT_TIMEOUT_MS);
    *settings = (struct dax_combo_global_settings){
        .timeout_ms = timeout_ms ? timeout_ms : CONFIG_ZMK_DAX_COMBO_DEFAULT_TIMEOUT_MS,
        .slow_release = slow_release_set && slow_release != 0,
        .max_combo = CONFIG_ZMK_DAX_COMBO_MAX_COMBOS,
        .require_prior_idle_ms =
            read_u16(&dax_combo_prior_idle, CONFIG_ZMK_DAX_COMBO_DEFAULT_REQUIRE_PRIOR_IDLE_MS),
    };
    return 0;
}

uint32_t dax_combo_max_count(void) { return CONFIG_ZMK_DAX_COMBO_MAX_COMBOS; }

int dax_combo_write(uint32_t index, const struct dax_combo_config *combo) {
    if (!combo || index >= CONFIG_ZMK_DAX_COMBO_MAX_COMBOS || combo->key_position_len < 1 ||
        combo->key_position_len > CONFIG_ZMK_DAX_COMBO_MAX_KEYS_PER_COMBO) {
        return -EINVAL;
    }
    for (uint8_t i = 0; i < combo->key_position_len; i++) {
        if (combo->key_positions[i] < 0 || combo->key_positions[i] > UINT16_MAX) {
            return -ERANGE;
        }
    }
    zmk_behavior_local_id_t behavior_id = zmk_behavior_get_local_id(combo->behavior.behavior_dev);
    if (behavior_id == 0 || behavior_id == UINT16_MAX) {
        return -ENODEV;
    }
    int ret = zmk_behavior_validate_binding(&combo->behavior);
    if (ret < 0) {
        return ret;
    }

    /* The name stays: it is written on its own. */
    struct record record = {0};
    struct record current;
    uint8_t buf[RECORD_MAX_SIZE];
    int len = dax_setting_get(&dax_combo_slots, index, buf, sizeof(buf));
    if (len >= 0 && decode(buf, len, &current) == 0) {
        memcpy(record.name, current.name, sizeof(record.name));
    }
    record.config = *combo;
    record.config.behavior_id = behavior_id;
    return write_record(index, &record);
}

int dax_combo_write_name(uint32_t index, const char *name) {
    if (!name || strlen(name) > DAX_COMBO_NAME_MAX) {
        return -EINVAL;
    }
    struct record record;
    int ret = read_record(index, &record);
    if (ret < 0) {
        return ret;
    }
    strcpy(record.name, name);
    return write_record(index, &record);
}

static int write_u16(const struct dax_setting *setting, uint16_t value) {
    uint8_t buf[sizeof(uint16_t)];
    sys_put_le16(value, buf);
    return dax_setting_set(setting, 0, buf, sizeof(buf));
}

int dax_combo_write_timeout_ms(uint16_t timeout_ms) {
    return timeout_ms == 0 ? -EINVAL : write_u16(&dax_combo_timeout, timeout_ms);
}

int dax_combo_write_slow_release(bool slow_release) {
    uint8_t value = slow_release;
    return dax_setting_set(&dax_combo_slow_release, 0, &value, 1);
}

int dax_combo_write_require_prior_idle_ms(uint16_t require_prior_idle_ms) {
    return write_u16(&dax_combo_prior_idle, require_prior_idle_ms);
}

int dax_combo_delete(uint32_t index) {
    struct record record;
    int ret = read_record(index, &record);
    if (ret < 0) {
        return ret;
    }
    record.config.enabled = false;
    return write_record(index, &record);
}

int dax_combo_reset(uint32_t index) {
    if (index >= CONFIG_ZMK_DAX_COMBO_MAX_COMBOS) {
        return -EINVAL;
    }
    return dax_setting_unset(&dax_combo_slots, index);
}

int dax_combo_save(uint32_t *written) { return dax_settings_save(DAX_COMBO_SUBSYSTEM_ID, written); }

int dax_combo_discard(uint32_t *changed) {
    return dax_settings_discard(DAX_COMBO_SUBSYSTEM_ID, changed);
}

uint32_t dax_combo_default_count(void) { return DAX_COMBO_DEFAULT_COUNT; }

bool dax_combo_has_default(uint32_t index) { return find_default(index) != NULL; }

int dax_combo_read_default(uint32_t index, struct dax_combo_config *combo) {
    if (!combo) {
        return -EINVAL;
    }
    const struct dax_combo_default *def = find_default(index);
    if (!def) {
        return -ENOENT;
    }
    *combo = def->config;
    combo->behavior_id = zmk_behavior_get_local_id(def->config.behavior.behavior_dev);
    return 0;
}

bool dax_combo_has_override(uint32_t index) {
    return index < CONFIG_ZMK_DAX_COMBO_MAX_COMBOS && dax_setting_is_set(&dax_combo_slots, index);
}

static int dax_combo_settings_listener(const zmk_event_t *eh) {
    const struct dax_setting_changed *changed = as_dax_setting_changed(eh);
    if (changed == NULL || strcmp(changed->owner, DAX_COMBO_SUBSYSTEM_ID) == 0) {
        dax_combo_engine_request_rebuild();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_combo_settings, dax_combo_settings_listener);
ZMK_SUBSCRIPTION(dax_combo_settings, dax_setting_changed);
ZMK_SUBSCRIPTION(dax_combo_settings, dax_settings_loaded);
