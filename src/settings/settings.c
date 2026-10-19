/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include <dax/settings.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DAX_SETTINGS_SUBTREE "dax"

ZMK_EVENT_IMPL(dax_settings_loaded);
ZMK_EVENT_IMPL(dax_setting_changed);

/* Guards every slot's state and bytes. A save lets go of it before settings_save_one, because
 * settings_load holds the settings lock while dax_settings_handle_set below takes this one. */
static K_MUTEX_DEFINE(state_lock);
/* One save at a time, so the scratch copy below is never shared. */
static K_MUTEX_DEFINE(save_lock);
static uint8_t save_scratch[CONFIG_ZMK_DAX_SETTINGS_VALUE_MAX_SIZE];

static uint8_t *live_bytes(const struct dax_setting *setting, uint16_t slot) {
    return setting->live + (size_t)slot * setting->size;
}

static uint8_t *saved_bytes(const struct dax_setting *setting, uint16_t slot) {
    return setting->saved + (size_t)slot * setting->size;
}

/* Whether RAM differs from what was last saved: a slot written back to its saved value is clean. */
static bool slot_dirty(const struct dax_setting *setting, uint16_t slot) {
    const struct dax_setting_slot *state = &setting->slots[slot];
    if (state->live_set != state->saved_set) {
        return true;
    }
    if (!state->live_set) {
        return false;
    }
    return state->live_len != state->saved_len ||
           memcmp(live_bytes(setting, slot), saved_bytes(setting, slot), state->live_len) != 0;
}

static void raise_changed(const char *owner) {
    raise_dax_setting_changed((struct dax_setting_changed){.owner = owner});
}

static int storage_name(const struct dax_setting *setting, uint16_t slot, char *name, size_t size) {
    int len = setting->count == 1
                  ? snprintf(name, size, DAX_SETTINGS_SUBTREE "/%s/%s", setting->owner,
                             setting->key)
                  : snprintf(name, size, DAX_SETTINGS_SUBTREE "/%s/%s/%u", setting->owner,
                             setting->key, slot);
    return len < 0 || (size_t)len >= size ? -ENAMETOOLONG : 0;
}

int dax_setting_get(const struct dax_setting *setting, uint16_t slot, void *buf, size_t buf_size) {
    if (!setting || slot >= setting->count) {
        return -EINVAL;
    }
    k_mutex_lock(&state_lock, K_FOREVER);
    const struct dax_setting_slot *state = &setting->slots[slot];
    int ret;
    if (!state->live_set) {
        ret = -ENOENT;
    } else if (state->live_len > buf_size) {
        ret = -ENOSPC;
    } else {
        memcpy(buf, live_bytes(setting, slot), state->live_len);
        ret = state->live_len;
    }
    k_mutex_unlock(&state_lock);
    return ret;
}

bool dax_setting_is_set(const struct dax_setting *setting, uint16_t slot) {
    if (!setting || slot >= setting->count) {
        return false;
    }
    k_mutex_lock(&state_lock, K_FOREVER);
    bool set = setting->slots[slot].live_set;
    k_mutex_unlock(&state_lock);
    return set;
}

int dax_setting_set(const struct dax_setting *setting, uint16_t slot, const void *data,
                    size_t len) {
    /* An empty value would be stored as a deletion: NVS treats a zero-length write as one. */
    if (!setting || slot >= setting->count || len == 0 || !data) {
        return -EINVAL;
    }
    if (len > setting->size) {
        return -EMSGSIZE;
    }
    k_mutex_lock(&state_lock, K_FOREVER);
    memcpy(live_bytes(setting, slot), data, len);
    setting->slots[slot].live_len = len;
    setting->slots[slot].live_set = true;
    k_mutex_unlock(&state_lock);
    raise_changed(setting->owner);
    return 0;
}

int dax_setting_unset(const struct dax_setting *setting, uint16_t slot) {
    if (!setting || slot >= setting->count) {
        return -EINVAL;
    }
    k_mutex_lock(&state_lock, K_FOREVER);
    setting->slots[slot].live_set = false;
    setting->slots[slot].live_len = 0;
    k_mutex_unlock(&state_lock);
    raise_changed(setting->owner);
    return 0;
}

bool dax_setting_is_dirty(const struct dax_setting *setting, uint16_t slot) {
    if (!setting || slot >= setting->count) {
        return false;
    }
    k_mutex_lock(&state_lock, K_FOREVER);
    bool dirty = slot_dirty(setting, slot);
    k_mutex_unlock(&state_lock);
    return dirty;
}

int dax_setting_check(const struct dax_setting *setting, uint16_t slot, const void *data,
                      size_t len) {
    if (!setting || !setting->type || slot >= setting->count || !data || len == 0 ||
        len > setting->size) {
        return -EINVAL;
    }
    const struct dax_setting_type *type = setting->type;
    const uint8_t *bytes = data;
    switch (type->kind) {
    case DAX_SETTING_INT32: {
        if (len != sizeof(int32_t)) {
            return -EINVAL;
        }
        int32_t value = (int32_t)sys_get_le32(bytes);
        if (value < type->min || value > type->max) {
            return -EINVAL;
        }
        break;
    }
    case DAX_SETTING_BOOL:
        if (len != 1 || bytes[0] > 1) {
            return -EINVAL;
        }
        break;
    case DAX_SETTING_BYTES:
        break;
    default:
        return -EINVAL;
    }
    return type->validate ? type->validate(slot, bytes, len) : 0;
}

int dax_setting_read(const struct dax_setting *setting, uint16_t slot, void *buf, size_t buf_size,
                     bool *unreadable) {
    if (!setting || !setting->type || slot >= setting->count) {
        return -EINVAL;
    }
    uint8_t value[DAX_SETTING_TYPED_MAX_SIZE];
    int len = dax_setting_get(setting, slot, value, sizeof(value));
    bool passed_over = false;
    if (len >= 0 && dax_setting_check(setting, slot, value, len) == 0) {
        if ((size_t)len > buf_size) {
            return -ENOSPC;
        }
        memcpy(buf, value, len);
    } else {
        /* A record too large to load is a set slot of length 0, and is passed over too. */
        passed_over = len >= 0;
        len = setting->type->default_value
                  ? setting->type->default_value(slot, buf, buf_size)
                  : -ENOENT;
    }
    if (unreadable) {
        *unreadable = passed_over;
    }
    return len;
}

int32_t dax_setting_read_int32(const struct dax_setting *setting, uint16_t slot,
                               int32_t fallback) {
    uint8_t value[sizeof(int32_t)];
    return dax_setting_read(setting, slot, value, sizeof(value), NULL) == sizeof(value)
               ? (int32_t)sys_get_le32(value)
               : fallback;
}

bool dax_setting_read_bool(const struct dax_setting *setting, uint16_t slot, bool fallback) {
    uint8_t value;
    return dax_setting_read(setting, slot, &value, 1, NULL) == 1 ? value != 0 : fallback;
}

static bool owned_by(const struct dax_setting *setting, const char *owner) {
    return strcmp(setting->owner, owner) == 0;
}

int dax_settings_save(const char *owner, uint32_t *written) {
    if (!owner) {
        return -EINVAL;
    }
    uint32_t count = 0;
    int first_error = 0;

    k_mutex_lock(&save_lock, K_FOREVER);
    STRUCT_SECTION_FOREACH(dax_setting, setting) {
        if (!owned_by(setting, owner)) {
            continue;
        }
        for (uint16_t slot = 0; slot < setting->count; slot++) {
            k_mutex_lock(&state_lock, K_FOREVER);
            bool dirty = slot_dirty(setting, slot);
            bool set = setting->slots[slot].live_set;
            uint16_t len = setting->slots[slot].live_len;
            if (dirty && set) {
                memcpy(save_scratch, live_bytes(setting, slot), len);
            }
            k_mutex_unlock(&state_lock);
            if (!dirty) {
                continue;
            }

            char name[SETTINGS_MAX_NAME_LEN + 1];
            int ret = storage_name(setting, slot, name, sizeof(name));
            if (ret == 0) {
                ret = set ? settings_save_one(name, save_scratch, len) : settings_delete(name);
            }
            if (ret < 0) {
                LOG_ERR("dax settings: saving %s failed (%d)", name, ret);
                first_error = first_error ?: ret;
                continue;
            }

            /* What was written becomes the saved value, even if RAM moved on meanwhile. */
            k_mutex_lock(&state_lock, K_FOREVER);
            struct dax_setting_slot *state = &setting->slots[slot];
            state->saved_set = set;
            state->saved_len = set ? len : 0;
            if (set) {
                memcpy(saved_bytes(setting, slot), save_scratch, len);
            }
            k_mutex_unlock(&state_lock);
            count++;
        }
    }
    k_mutex_unlock(&save_lock);

    if (written) {
        *written = count;
    }
    if (count > 0) {
        raise_changed(owner);
    }
    return first_error;
}

int dax_settings_discard(const char *owner, uint32_t *changed) {
    if (!owner) {
        return -EINVAL;
    }
    uint32_t count = 0;
    k_mutex_lock(&state_lock, K_FOREVER);
    STRUCT_SECTION_FOREACH(dax_setting, setting) {
        if (!owned_by(setting, owner)) {
            continue;
        }
        for (uint16_t slot = 0; slot < setting->count; slot++) {
            if (!slot_dirty(setting, slot)) {
                continue;
            }
            struct dax_setting_slot *state = &setting->slots[slot];
            state->live_set = state->saved_set;
            state->live_len = state->saved_len;
            memcpy(live_bytes(setting, slot), saved_bytes(setting, slot), state->saved_len);
            count++;
        }
    }
    k_mutex_unlock(&state_lock);

    if (changed) {
        *changed = count;
    }
    raise_changed(owner);
    return 0;
}

/* Finds the setting and slot a stored "<owner>/<key>" or "<owner>/<key>/<slot>" belongs to. */
static const struct dax_setting *find_stored(const char *name, uint16_t *slot) {
    STRUCT_SECTION_FOREACH(dax_setting, setting) {
        size_t owner_len = strlen(setting->owner);
        size_t key_len = strlen(setting->key);
        if (strncmp(name, setting->owner, owner_len) != 0 || name[owner_len] != '/') {
            continue;
        }
        const char *key = name + owner_len + 1;
        if (strncmp(key, setting->key, key_len) != 0) {
            continue;
        }
        const char *rest = key + key_len;
        if (setting->count == 1 && *rest == '\0') {
            *slot = 0;
            return setting;
        }
        if (setting->count > 1 && *rest == '/') {
            /* Only the spelling a save writes: "07" or "+7" would be a second record for slot 7
             * that a reset could never delete. */
            const char *digits = rest + 1;
            bool canonical = digits[0] >= '1' && digits[0] <= '9';
            canonical = canonical || (digits[0] == '0' && digits[1] == '\0');
            for (const char *c = digits; canonical && *c != '\0'; c++) {
                canonical = *c >= '0' && *c <= '9';
            }
            unsigned long index = canonical ? strtoul(digits, NULL, 10) : ULONG_MAX;
            if (index < setting->count) {
                *slot = index;
                return setting;
            }
        }
    }
    return NULL;
}

static int dax_settings_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                   void *cb_arg) {
    /* Zephyr passes no name for a record called just "dax". */
    if (!name) {
        LOG_WRN("dax settings: ignoring %s", DAX_SETTINGS_SUBTREE);
        return 0;
    }
    uint16_t slot;
    const struct dax_setting *setting = find_stored(name, &slot);
    if (!setting) {
        /* A setting a newer or older firmware had: left in flash, not loaded. */
        LOG_WRN("dax settings: ignoring %s/%s", DAX_SETTINGS_SUBTREE, name);
        return 0;
    }
    if (len > setting->size) {
        /* Kept as a slot that is set but holds nothing a feature can read, as a record of the
         * wrong version is, so a reset and a save delete it. */
        LOG_WRN("dax settings: %s/%s is %u bytes, more than %u; unreadable", DAX_SETTINGS_SUBTREE,
                name, (unsigned)len, setting->size);
        k_mutex_lock(&state_lock, K_FOREVER);
        setting->slots[slot] = (struct dax_setting_slot){.live_set = true, .saved_set = true};
        k_mutex_unlock(&state_lock);
        return 0;
    }

    /* Read aside first, so a failed read leaves the slot as it was. */
    uint8_t value[CONFIG_ZMK_DAX_SETTINGS_VALUE_MAX_SIZE];
    ssize_t read = read_cb(cb_arg, value, len);
    if (read < 0) {
        return read;
    }
    k_mutex_lock(&state_lock, K_FOREVER);
    struct dax_setting_slot *state = &setting->slots[slot];
    *state = (struct dax_setting_slot){
        .live_len = read, .saved_len = read, .live_set = true, .saved_set = true};
    memcpy(saved_bytes(setting, slot), value, read);
    memcpy(live_bytes(setting, slot), value, read);
    k_mutex_unlock(&state_lock);
    return 0;
}

static bool loaded;

static int dax_settings_handle_commit(void) {
    if (!loaded) {
        loaded = true;
        raise_dax_settings_loaded((struct dax_settings_loaded){});
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dax_settings, DAX_SETTINGS_SUBTREE, NULL, dax_settings_handle_set,
                               dax_settings_handle_commit, NULL);

#if IS_ENABLED(CONFIG_ZMK_DAX_SETTINGS_TEST)
int dax_settings_test_reload(const char *owner) {
    k_mutex_lock(&state_lock, K_FOREVER);
    STRUCT_SECTION_FOREACH(dax_setting, setting) {
        if (owned_by(setting, owner)) {
            memset(setting->slots, 0, setting->count * sizeof(setting->slots[0]));
        }
    }
    k_mutex_unlock(&state_lock);

    char subtree[SETTINGS_MAX_NAME_LEN + 1];
    snprintf(subtree, sizeof(subtree), DAX_SETTINGS_SUBTREE "/%s", owner);
    int ret = settings_load_subtree(subtree);
    raise_changed(owner);
    return ret;
}
#endif
