/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * The dax settings layer: values a feature edits at runtime, kept in RAM until saved. A setting
 * is an owner (the feature's subsystem id), a key and slots of up to `size` bytes, or unset. The
 * layer keeps bytes only: a feature decodes them, and gives its own default for an unset slot. A
 * value is not decoded while settings load, only when a feature reads it. In flash a slot is
 * "dax/<owner>/<key>", or "dax/<owner>/<key>/<slot>" when there are several.
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

struct dax_setting {
    const char *owner;
    const char *key;
    uint16_t count;
    uint16_t size;
    uint8_t *live;
    uint8_t *saved;
    struct dax_setting_slot *slots;
};

#define DAX_SETTING_DEFINE(_name, _owner, _key, _count, _size)                                     \
    BUILD_ASSERT((_count) >= 1, "a dax setting needs at least one slot");                          \
    BUILD_ASSERT((_size) >= 1 && (_size) <= CONFIG_ZMK_DAX_SETTINGS_VALUE_MAX_SIZE,               \
                 "a dax setting slot must fit CONFIG_ZMK_DAX_SETTINGS_VALUE_MAX_SIZE");            \
    BUILD_ASSERT(sizeof("dax/" _owner "/" _key "/65535") <= SETTINGS_MAX_NAME_LEN + 1,             \
                 "the setting's name is too long for Zephyr settings");                           \
    static uint8_t _name##_live[(_count) * (_size)];                                               \
    static uint8_t _name##_saved[(_count) * (_size)];                                              \
    static struct dax_setting_slot _name##_slots[_count];                                          \
    const STRUCT_SECTION_ITERABLE(dax_setting, _name) = {                                                \
        .owner = _owner,                                                                           \
        .key = _key,                                                                               \
        .count = _count,                                                                           \
        .size = _size,                                                                             \
        .live = _name##_live,                                                                      \
        .saved = _name##_saved,                                                                    \
        .slots = _name##_slots,                                                                    \
    }

/* The slot's length, or -ENOENT when it is unset, -ENOSPC when `buf` is too small. */
int dax_setting_get(const struct dax_setting *setting, uint16_t slot, void *buf, size_t buf_size);
bool dax_setting_is_set(const struct dax_setting *setting, uint16_t slot);
/* RAM only; the slot differs from flash until the owner is saved or discarded. */
int dax_setting_set(const struct dax_setting *setting, uint16_t slot, const void *data, size_t len);
int dax_setting_unset(const struct dax_setting *setting, uint16_t slot);

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

/* Raised after a write, a save or a discard changed an owner's values, from the thread that did it. */
struct dax_setting_changed {
    const char *owner;
};
ZMK_EVENT_DECLARE(dax_setting_changed);
