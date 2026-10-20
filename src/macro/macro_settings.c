/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 *
 * Adapted from cormoran/zmk-feature-runtime-macro (src/runtime_macro_dt_defaults.c at 3853096):
 * the devicetree defaults and their encoder, under the dax_macro names. The slots are stored in
 * the dax settings layer, one record each, so a macro is never half in flash:
 *
 *   0      name length n (1..CONFIG_ZMK_DAX_MACRO_NAME_MAX)
 *   1..n   name, UTF-8 without a terminator
 *   n+1..  body (macro_body.h), whose first byte is its version
 *
 * An unset slot shows its default, encoded when it is read: ZMK's table of local ids is itself
 * read from settings and may not be there while settings load. A default cannot be deleted; a
 * reset shows it again. tap_ms is a uint16, little-endian.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <dax/macro.h>
#include <dax/settings.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>

#include "macro_body.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define RECORD_MAX_SIZE (1 + CONFIG_ZMK_DAX_MACRO_NAME_MAX + CONFIG_ZMK_DAX_MACRO_MAX_BYTES)
#define TAP_MS_MAX 10000

DAX_SETTING_DEFINE_SLOTS(dax_macro_slots, DAX_MACRO_SUBSYSTEM_ID, "macro",
                         CONFIG_ZMK_DAX_MACRO_COUNT, RECORD_MAX_SIZE);
DAX_SETTING_DEFINE(dax_macro_tap, DAX_MACRO_SUBSYSTEM_ID, "tap_ms", sizeof(uint16_t));

struct macro_default {
    uint32_t slot;
    const char *name;
    uint32_t wait_ms;
    const struct zmk_behavior_binding *bindings;
    size_t bindings_len;
};

#define DT_DRV_COMPAT dax_runtime_macro_defaults

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
             "only one dax,runtime-macro-defaults node is read");

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) && DT_INST_CHILD_NUM(0) > 0

#define DEFAULT_NAME(n) DT_PROP_OR(n, macro_name, DT_NODE_FULL_NAME(n))

#define DEFAULT_VALIDATE(n)                                                                        \
    BUILD_ASSERT(DT_PROP(n, slot) < CONFIG_ZMK_DAX_MACRO_COUNT,                                    \
                 "dax macro default slot is out of range");                                        \
    BUILD_ASSERT(sizeof(DEFAULT_NAME(n)) >= 2 &&                                                   \
                     sizeof(DEFAULT_NAME(n)) - 1 <= CONFIG_ZMK_DAX_MACRO_NAME_MAX,                 \
                 "dax macro default name is empty or longer than CONFIG_ZMK_DAX_MACRO_NAME_MAX");

DT_INST_FOREACH_CHILD(0, DEFAULT_VALIDATE)

#define DEFAULT_SLOT_BIT(n) (DT_PROP(n, slot) < 32 ? BIT(DT_PROP(n, slot)) : 0)
BUILD_ASSERT(__builtin_popcount(DT_INST_FOREACH_CHILD_SEP(0, DEFAULT_SLOT_BIT, (|))) ==
                 DT_INST_CHILD_NUM(0),
             "two dax macro defaults have the same slot");

#define DEFAULT_BINDINGS(n)                                                                        \
    static const struct zmk_behavior_binding default_bindings_##n[] = {                            \
        LISTIFY(DT_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), n)};

DT_INST_FOREACH_CHILD(0, DEFAULT_BINDINGS)

#define DEFAULT_INST(n)                                                                            \
    {                                                                                              \
        .slot = DT_PROP(n, slot),                                                                  \
        .name = DEFAULT_NAME(n),                                                                   \
        .wait_ms = DT_PROP(n, wait_ms),                                                            \
        .bindings = default_bindings_##n,                                                          \
        .bindings_len = ARRAY_SIZE(default_bindings_##n),                                          \
    },

static const struct macro_default macro_defaults[] = {DT_INST_FOREACH_CHILD(0, DEFAULT_INST)};

#define DEFAULT_COUNT ARRAY_SIZE(macro_defaults)

#else

static const struct macro_default macro_defaults[1];

#define DEFAULT_COUNT 0

#endif

/* The control behaviors of ZMK's behaviors.dtsi, guarded so a keymap that leaves them out still
 * builds. */
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_macro_control_mode_tap)
#define TAP_MODE_NAME DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_tap))
#else
#define TAP_MODE_NAME NULL
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_macro_control_mode_press)
#define PRESS_MODE_NAME DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_press))
#else
#define PRESS_MODE_NAME NULL
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_macro_control_mode_release)
#define RELEASE_MODE_NAME DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_mode_release))
#else
#define RELEASE_MODE_NAME NULL
#endif
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_macro_control_wait_time)
#define WAIT_TIME_NAME DEVICE_DT_NAME(DT_INST(0, zmk_macro_control_wait_time))
#else
#define WAIT_TIME_NAME NULL
#endif
#define KP_NAME DEVICE_DT_NAME(DT_NODELABEL(kp))

static const struct macro_default *find_default(uint32_t slot) {
    for (size_t i = 0; i < DEFAULT_COUNT; i++) {
        if (macro_defaults[i].slot == slot) {
            return &macro_defaults[i];
        }
    }
    return NULL;
}

struct encoder {
    uint8_t *body;
    size_t size;
    size_t len;
    struct dax_macro_step keys;
};

static int flush_keys(struct encoder *e) {
    if (e->keys.key_count == 0) {
        return 0;
    }
    int ret = dax_macro_body_put_step(e->body, e->size, &e->len, &e->keys);
    e->keys.key_count = 0;
    return ret;
}

static int put_step(struct encoder *e, const struct dax_macro_step *step) {
    int ret = flush_keys(e);
    return ret ?: dax_macro_body_put_step(e->body, e->size, &e->len, step);
}

/* Unlike the runtime macro, a run of keys is split where one step of the RPC ends, so a client can
 * read every default back. */
static int put_key(struct encoder *e, uint8_t packed) {
    if (e->keys.key_count == DAX_MACRO_KEY_TAP_SEQUENCE_MAX) {
        int ret = flush_keys(e);
        if (ret < 0) {
            return ret;
        }
    }
    e->keys.keys[e->keys.key_count++] = packed;
    return 0;
}

static bool is(const struct zmk_behavior_binding *binding, const char *name) {
    return name && strcmp(binding->behavior_dev, name) == 0;
}

/* Taps of &kp that fit a packed key go into one key tap sequence, and wait-ms only separates the
 * steps that are not packed, as the runtime macro module encodes its defaults. */
static int encode_default(const struct macro_default *def, uint8_t *body, size_t size) {
    struct encoder e = {
        .body = body,
        .size = size,
        .keys = {.kind = DAX_MACRO_STEP_KEY_TAP_SEQUENCE},
    };
    if (size < 1) {
        return -EMSGSIZE;
    }
    body[e.len++] = DAX_MACRO_BODY_VERSION;

    enum dax_macro_step_kind mode = DAX_MACRO_STEP_TAP;
    bool wait_before_next = false;
    for (size_t i = 0; i < def->bindings_len; i++) {
        const struct zmk_behavior_binding *binding = &def->bindings[i];
        int ret;
        if (is(binding, TAP_MODE_NAME) || is(binding, PRESS_MODE_NAME) ||
            is(binding, RELEASE_MODE_NAME)) {
            mode = is(binding, TAP_MODE_NAME)     ? DAX_MACRO_STEP_TAP
                   : is(binding, PRESS_MODE_NAME) ? DAX_MACRO_STEP_DOWN
                                                  : DAX_MACRO_STEP_UP;
            continue;
        }
        if (is(binding, WAIT_TIME_NAME)) {
            ret = put_step(&e, &(struct dax_macro_step){.kind = DAX_MACRO_STEP_DELAY,
                                                        .delay_ms = binding->param1});
            if (ret < 0) {
                return ret;
            }
            wait_before_next = false;
            continue;
        }

        uint8_t packed;
        if (mode == DAX_MACRO_STEP_TAP && is(binding, KP_NAME) &&
            dax_macro_pack_key(binding->param1, &packed) == 0) {
            ret = put_key(&e, packed);
            if (ret < 0) {
                return ret;
            }
            wait_before_next = false;
            continue;
        }

        if (wait_before_next && def->wait_ms > 0) {
            ret = put_step(&e, &(struct dax_macro_step){.kind = DAX_MACRO_STEP_DELAY,
                                                        .delay_ms = def->wait_ms});
            if (ret < 0) {
                return ret;
            }
        }
        zmk_behavior_local_id_t id = zmk_behavior_get_local_id(binding->behavior_dev);
        if (id == UINT16_MAX) {
            return -ENODEV;
        }
        ret = put_step(&e, &(struct dax_macro_step){.kind = mode,
                                                    .behavior_id = id,
                                                    .param1 = binding->param1,
                                                    .param2 = binding->param2});
        if (ret < 0) {
            return ret;
        }
        wait_before_next = true;
    }
    int ret = flush_keys(&e);
    return ret < 0 ? ret : (int)e.len;
}

static bool record_readable(const uint8_t *record, size_t len) {
    if (len < 1) {
        return false;
    }
    size_t name_len = record[0];
    if (name_len < 1 || name_len > CONFIG_ZMK_DAX_MACRO_NAME_MAX || len < 1 + name_len + 1 ||
        len - 1 - name_len > CONFIG_ZMK_DAX_MACRO_MAX_BYTES ||
        memchr(&record[1], '\0', name_len) != NULL) {
        return false;
    }
    return dax_macro_body_check(&record[1 + name_len], len - 1 - name_len) == 0;
}

/* What the slot shows, as a record: its own, or its default's. Its length, or -ENOENT. */
static int read_record(uint32_t slot, uint8_t *record) {
    if (slot >= CONFIG_ZMK_DAX_MACRO_COUNT) {
        return -ENOENT;
    }
    int len = dax_setting_get(&dax_macro_slots, slot, record, RECORD_MAX_SIZE);
    if (len >= 0) {
        if (record_readable(record, len)) {
            return len;
        }
        /* Its default shows in its place, as overridden, so a reset still reaches the record. */
        LOG_DBG("dax macro: slot %u holds a record this firmware cannot read", slot);
    } else if (len != -ENOENT) {
        return len;
    }
    const struct macro_default *def = find_default(slot);
    if (!def) {
        return -ENOENT;
    }
    size_t name_len = strlen(def->name);
    record[0] = name_len;
    memcpy(&record[1], def->name, name_len);
    int body_len = encode_default(def, &record[1 + name_len], CONFIG_ZMK_DAX_MACRO_MAX_BYTES);
    if (body_len < 0) {
        LOG_WRN("dax macro: the default %s cannot be encoded (%d)", def->name, body_len);
        return -ENOENT;
    }
    return 1 + name_len + body_len;
}

/* An edit that changes nothing writes nothing, so it does not leave a default unsaved. */
static int store(uint32_t slot, const uint8_t *current, int current_len, const uint8_t *record,
                 size_t len) {
    if (current_len == (int)len && memcmp(current, record, len) == 0) {
        return 0;
    }
    return dax_setting_set(&dax_macro_slots, slot, record, len);
}

int dax_macro_read(uint32_t slot, char *name, size_t name_size, uint8_t *body, size_t body_size) {
    uint8_t record[RECORD_MAX_SIZE];
    int len = read_record(slot, record);
    if (len < 0) {
        return len;
    }
    size_t name_len = record[0];
    size_t body_len = len - 1 - name_len;
    if ((name && name_size < name_len + 1) || (body && body_size < body_len)) {
        return -ENOSPC;
    }
    if (name) {
        memcpy(name, &record[1], name_len);
        name[name_len] = '\0';
    }
    if (body) {
        memcpy(body, &record[1 + name_len], body_len);
    }
    return body_len;
}

enum dax_macro_source dax_macro_source(uint32_t slot) {
    uint8_t record[RECORD_MAX_SIZE];
    if (read_record(slot, record) < 0) {
        return DAX_MACRO_SOURCE_EMPTY;
    }
    bool written = dax_setting_is_set(&dax_macro_slots, slot);
    if (!find_default(slot)) {
        return DAX_MACRO_SOURCE_RUNTIME;
    }
    return written ? DAX_MACRO_SOURCE_OVERRIDDEN : DAX_MACRO_SOURCE_DEFAULT;
}

bool dax_macro_is_unsaved(uint32_t slot) { return dax_setting_is_dirty(&dax_macro_slots, slot); }

uint16_t dax_macro_tap_ms(void) {
    uint8_t buf[sizeof(uint16_t)];
    bool stored = dax_setting_get(&dax_macro_tap, 0, buf, sizeof(buf)) == sizeof(buf);
    /* A record of another firmware can hold more than a write lets through. */
    return stored && sys_get_le16(buf) <= TAP_MS_MAX ? sys_get_le16(buf)
                                                      : CONFIG_ZMK_DAX_MACRO_DEFAULT_TAP_MS;
}

static int check_name(const char *name) {
    size_t len = name ? strlen(name) : 0;
    if (len == 0) {
        return -EINVAL;
    }
    return len > CONFIG_ZMK_DAX_MACRO_NAME_MAX ? -EMSGSIZE : 0;
}

/* -EEXIST when a slot other than `except` shows a macro with this name. */
static int check_unique(const char *name, uint32_t except) {
    char shown[CONFIG_ZMK_DAX_MACRO_NAME_MAX + 1];
    for (uint32_t slot = 0; slot < CONFIG_ZMK_DAX_MACRO_COUNT; slot++) {
        if (slot != except && dax_macro_read(slot, shown, sizeof(shown), NULL, 0) >= 0 &&
            strcmp(shown, name) == 0) {
            return -EEXIST;
        }
    }
    return 0;
}

static size_t put_name(uint8_t *record, const char *name) {
    size_t len = strlen(name);
    record[0] = len;
    memcpy(&record[1], name, len);
    return 1 + len;
}

int dax_macro_create(const char *name, uint32_t *slot) {
    int ret = check_name(name);
    ret = ret ?: check_unique(name, UINT32_MAX);
    if (ret < 0) {
        return ret;
    }
    uint8_t record[RECORD_MAX_SIZE];
    for (uint32_t candidate = 0; candidate < CONFIG_ZMK_DAX_MACRO_COUNT; candidate++) {
        /* A slot with a default is passed over even when it shows nothing: a macro made there
         * could not be deleted, and a reset would put the default back over it. */
        if (find_default(candidate) || read_record(candidate, record) >= 0) {
            continue;
        }
        size_t len = put_name(record, name);
        record[len++] = DAX_MACRO_BODY_VERSION;
        ret = dax_setting_set(&dax_macro_slots, candidate, record, len);
        if (ret == 0 && slot) {
            *slot = candidate;
        }
        return ret;
    }
    return -ENOSPC;
}

int dax_macro_rename(uint32_t slot, const char *name) {
    uint8_t current[RECORD_MAX_SIZE];
    int len = read_record(slot, current);
    if (len < 0) {
        return len;
    }
    int ret = check_name(name);
    ret = ret ?: check_unique(name, slot);
    if (ret < 0) {
        return ret;
    }
    uint8_t record[RECORD_MAX_SIZE];
    size_t name_len = current[0];
    size_t body_len = len - 1 - name_len;
    size_t offset = put_name(record, name);
    memcpy(&record[offset], &current[1 + name_len], body_len);
    return store(slot, current, len, record, offset + body_len);
}

int dax_macro_delete(uint32_t slot) {
    uint8_t current[RECORD_MAX_SIZE];
    int len = read_record(slot, current);
    if (len < 0) {
        return len;
    }
    return find_default(slot) ? -EPERM : dax_setting_unset(&dax_macro_slots, slot);
}

int dax_macro_reset(uint32_t slot) {
    return slot < CONFIG_ZMK_DAX_MACRO_COUNT ? dax_setting_unset(&dax_macro_slots, slot) : -ENOENT;
}

/* Any reason this build cannot play a step is -EINVAL. */
static int check_step(const struct dax_macro_step *step) {
    struct zmk_behavior_binding binding;
    switch (step->kind) {
    case DAX_MACRO_STEP_DOWN:
    case DAX_MACRO_STEP_UP:
    case DAX_MACRO_STEP_TAP:
        return dax_macro_step_binding(step, &binding) < 0 ? -EINVAL : 0;
    case DAX_MACRO_STEP_DELAY:
        return 0;
    case DAX_MACRO_STEP_KEY_TAP_SEQUENCE:
        if (step->key_count > DAX_MACRO_KEY_TAP_SEQUENCE_MAX) {
            return -EINVAL;
        }
        for (uint8_t i = 0; i < step->key_count; i++) {
            if (dax_macro_key_binding(step->keys[i], &binding) < 0) {
                return -EINVAL;
            }
        }
        return 0;
    default:
        return -EINVAL;
    }
}

/* Where the first `count` steps of a readable body end; -ENOENT when it has fewer. */
static int steps_end(const uint8_t *body, size_t len, uint32_t count) {
    struct dax_macro_step step;
    size_t offset = 1;
    for (uint32_t i = 0; i < count; i++) {
        int ret = dax_macro_next_step(body, len, &offset, &step);
        if (ret < 0) {
            return ret;
        }
    }
    return offset;
}

/* Puts `step` in place of the steps from `index` to `end_index`, so one step is replaced when
 * they differ by one and the step is appended when both are the count. */
static int splice_step(uint32_t slot, uint32_t index, uint32_t end_index,
                       const struct dax_macro_step *step) {
    uint8_t current[RECORD_MAX_SIZE];
    int len = read_record(slot, current);
    if (len < 0) {
        return len;
    }
    int ret = check_step(step);
    if (ret < 0) {
        return ret;
    }
    size_t head = 1 + current[0];
    const uint8_t *body = &current[head];
    size_t body_len = len - head;
    int start = steps_end(body, body_len, index);
    int end = start < 0 ? start : steps_end(body, body_len, end_index);
    if (end < 0) {
        return end;
    }

    uint8_t record[RECORD_MAX_SIZE];
    memcpy(record, current, head + start);
    size_t offset = head + start;
    ret = dax_macro_body_put_step(record, head + CONFIG_ZMK_DAX_MACRO_MAX_BYTES, &offset, step);
    if (ret < 0) {
        return ret;
    }
    size_t rest = body_len - end;
    if (offset + rest > head + CONFIG_ZMK_DAX_MACRO_MAX_BYTES) {
        return -EMSGSIZE;
    }
    memcpy(&record[offset], &body[end], rest);
    return store(slot, current, len, record, offset + rest);
}

int dax_macro_set_step(uint32_t slot, uint32_t index, const struct dax_macro_step *step) {
    return index == UINT32_MAX ? -ENOENT : splice_step(slot, index, index + 1, step);
}

int dax_macro_append_step(uint32_t slot, const struct dax_macro_step *step) {
    uint8_t current[RECORD_MAX_SIZE];
    int len = read_record(slot, current);
    if (len < 0) {
        return len;
    }
    size_t head = 1 + current[0];
    uint32_t count = 0;
    size_t offset = 1;
    struct dax_macro_step existing;
    while (dax_macro_next_step(&current[head], len - head, &offset, &existing) == 0) {
        count++;
    }
    return splice_step(slot, count, count, step);
}

int dax_macro_set_step_count(uint32_t slot, uint32_t count) {
    uint8_t current[RECORD_MAX_SIZE];
    int len = read_record(slot, current);
    if (len < 0) {
        return len;
    }
    size_t head = 1 + current[0];
    int end = steps_end(&current[head], len - head, count);
    if (end < 0) {
        return -EINVAL;
    }
    uint8_t record[RECORD_MAX_SIZE];
    memcpy(record, current, head + end);
    return store(slot, current, len, record, head + end);
}

int dax_macro_write_tap_ms(uint16_t tap_ms) {
    if (tap_ms > TAP_MS_MAX) {
        return -EINVAL;
    }
    uint8_t buf[sizeof(uint16_t)];
    sys_put_le16(tap_ms, buf);
    return dax_setting_set(&dax_macro_tap, 0, buf, sizeof(buf));
}

int dax_macro_save(uint32_t *written) { return dax_settings_save(DAX_MACRO_SUBSYSTEM_ID, written); }

int dax_macro_discard(uint32_t *changed) {
    return dax_settings_discard(DAX_MACRO_SUBSYSTEM_ID, changed);
}
