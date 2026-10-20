/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * The dax settings layer: values a feature edits at runtime, kept in RAM until saved. A setting
 * is an owner (the feature's subsystem id), a key and slots of up to `size` bytes, or unset. The
 * layer keeps bytes only: a feature decodes them, and gives its own default for an unset slot. A
 * value is not decoded while settings load, only when a feature reads it. In flash a setting
 * defined with one value is "dax/<owner>/<key>"; each slot of a setting defined with slots is
 * "dax/<owner>/<key>/<slot>" even when there is one, so a count from the devicetree or Kconfig
 * that changes to or from 1 still finds what was stored.
 *
 * A typed setting also says what its bytes are and what an unset slot means, so the dax_settings
 * Studio RPC can list, check and write it without knowing the feature. An untyped one (a combo
 * record) is reached only through its feature's own RPC.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>

struct dax_setting_slot {
    uint16_t live_len;
    uint16_t saved_len;
    bool live_set;
    bool saved_set;
};

enum dax_setting_kind {
    /* Four bytes, little-endian, from min to max. */
    DAX_SETTING_INT32,
    /* One byte, 0 or 1. */
    DAX_SETTING_BOOL,
    /* One to size bytes, read by the owner. */
    DAX_SETTING_BYTES,
};

/* The most bytes a typed slot holds, so a list response stays a fixed size. */
#define DAX_SETTING_TYPED_MAX_SIZE 32

struct dax_setting_type {
    enum dax_setting_kind kind;
    int32_t min;
    int32_t max;
    /* The value is always the default, shown to a client only: a stored one is passed over, and
     * the Studio RPC refuses writes and resets. */
    bool read_only;
    /* Writes the slot's default to buf and returns its length, or -ENOENT when it has none. A
     * slot with no value and no default reads as empty. Optional. */
    int (*default_value)(uint16_t slot, uint8_t *buf, size_t size);
    /* Refuses with -EINVAL what the kind lets through but the owner cannot use. Optional. */
    int (*validate)(uint16_t slot, const uint8_t *data, size_t len);
};

struct dax_setting {
    const char *owner;
    const char *key;
    uint16_t count;
    /* Defined with slots: every slot is stored under its number, even when there is one. */
    bool indexed;
    uint16_t size;
    uint8_t *live;
    uint8_t *saved;
    struct dax_setting_slot *slots;
    /* NULL for an untyped setting. */
    const struct dax_setting_type *type;
};

/* One value, slot 0. */
#define DAX_SETTING_DEFINE(_name, _owner, _key, _size)                                             \
    Z_DAX_SETTING_DEFINE(_name, _owner, _key, 1, false, _size, NULL)
#define DAX_SETTING_DEFINE_SLOTS(_name, _owner, _key, _count, _size)                               \
    Z_DAX_SETTING_DEFINE(_name, _owner, _key, _count, true, _size, NULL)

#define DAX_SETTING_DEFINE_TYPED(_name, _owner, _key, _size, _type)                                \
    Z_DAX_SETTING_TYPED(_name, _owner, _key, 1, false, _size, _type)
#define DAX_SETTING_DEFINE_TYPED_SLOTS(_name, _owner, _key, _count, _size, _type)                  \
    Z_DAX_SETTING_TYPED(_name, _owner, _key, _count, true, _size, _type)

#define Z_DAX_SETTING_TYPED(_name, _owner, _key, _count, _indexed, _size, _type)                   \
    BUILD_ASSERT((_size) <= DAX_SETTING_TYPED_MAX_SIZE,                                            \
                 "a typed dax setting slot must fit DAX_SETTING_TYPED_MAX_SIZE");                  \
    Z_DAX_SETTING_DEFINE(_name, _owner, _key, _count, _indexed, _size, _type)

#define Z_DAX_SETTING_DEFINE(_name, _owner, _key, _count, _indexed, _size, _type)                  \
    BUILD_ASSERT((_count) >= 1, "a dax setting needs at least one slot");                          \
    BUILD_ASSERT((_size) >= 1 && (_size) <= CONFIG_ZMK_DAX_SETTINGS_VALUE_MAX_SIZE,               \
                 "a dax setting slot must fit CONFIG_ZMK_DAX_SETTINGS_VALUE_MAX_SIZE");            \
    BUILD_ASSERT(sizeof("dax/" _owner "/" _key "/65535") <= SETTINGS_MAX_NAME_LEN + 1,             \
                 "the setting's name is too long for Zephyr settings");                           \
    static uint8_t _name##_live[(_count) * (_size)];                                               \
    static uint8_t _name##_saved[(_count) * (_size)];                                              \
    static struct dax_setting_slot _name##_slots[_count];                                          \
    const STRUCT_SECTION_ITERABLE(dax_setting, _name) = {                                          \
        .owner = _owner,                                                                           \
        .key = _key,                                                                               \
        .count = _count,                                                                           \
        .indexed = _indexed,                                                                       \
        .size = _size,                                                                             \
        .live = _name##_live,                                                                      \
        .saved = _name##_saved,                                                                    \
        .slots = _name##_slots,                                                                    \
        .type = _type,                                                                             \
    }

/* The slot's length, or -ENOENT when it is unset, -ENOSPC when `buf` is too small. */
int dax_setting_get(const struct dax_setting *setting, uint16_t slot, void *buf, size_t buf_size);
bool dax_setting_is_set(const struct dax_setting *setting, uint16_t slot);
/* RAM only; the slot differs from flash until the owner is saved or discarded. `len` is at least
 * 1: an empty value would be stored as a deletion. */
int dax_setting_set(const struct dax_setting *setting, uint16_t slot, const void *data, size_t len);
int dax_setting_unset(const struct dax_setting *setting, uint16_t slot);
/* Whether the slot differs from what was last saved. */
bool dax_setting_is_dirty(const struct dax_setting *setting, uint16_t slot);

/* For a typed setting: 0 when data is a value the slot may hold, -EINVAL otherwise. */
int dax_setting_check(const struct dax_setting *setting, uint16_t slot, const void *data,
                      size_t len);
/* For a typed setting: the slot's value if it holds one its type accepts, else its default. The
 * length, or -ENOENT when there is neither; `unreadable` (may be NULL) says that a stored value
 * was passed over. */
int dax_setting_read(const struct dax_setting *setting, uint16_t slot, void *buf, size_t buf_size,
                     bool *unreadable);
/* The INT32 or BOOL value that dax_setting_read gives, or fallback when there is none. */
int32_t dax_setting_read_int32(const struct dax_setting *setting, uint16_t slot, int32_t fallback);
bool dax_setting_read_bool(const struct dax_setting *setting, uint16_t slot, bool fallback);

/* Writes the owner's slots that differ from what was last saved, and counts them in `written`.
 * Each slot that is written is saved on its own; the first error is returned. */
int dax_settings_save(const char *owner, uint32_t *written);
/* Puts the owner's slots back to what was last saved, and counts the ones that changed. */
int dax_settings_discard(const char *owner, uint32_t *changed);

#if IS_ENABLED(CONFIG_ZMK_DAX_SETTINGS_TEST)
/* Forgets the owner's RAM and reads its slots from flash again, as at boot. Tests only. */
int dax_settings_test_reload(const char *owner);
#endif

/* Raised once, when the settings have loaded at boot. */
struct dax_settings_loaded {};
ZMK_EVENT_DECLARE(dax_settings_loaded);

/* Raised after a write, a save or a discard changed an owner's values, on the writing thread. */
struct dax_setting_changed {
    const char *owner;
};
ZMK_EVENT_DECLARE(dax_setting_changed);
