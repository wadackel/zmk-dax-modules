/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <dax/settings.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DAX_SETTINGS_SUBTREE "dax"

ZMK_EVENT_IMPL(dax_settings_loaded);
ZMK_EVENT_IMPL(dax_setting_changed);

/* Guards every slot's state and bytes. Never held across a settings_* call: settings_load holds
 * its own lock while it calls dax_settings_set below, which takes this one. */
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
                  ? snprintf(name, size, DAX_SETTINGS_SUBTREE "/%s/%s", setting->owner, setting->key)
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

int dax_setting_set(const struct dax_setting *setting, uint16_t slot, const void *data, size_t len) {
    if (!setting || slot >= setting->count || (len > 0 && !data)) {
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

/* Finds the setting and slot a stored name ("<owner>/<key>" or "<owner>/<key>/<slot>") belongs to. */
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
        if (setting->count > 1 && *rest == '/' && rest[1] != '\0') {
            char *end;
            unsigned long index = strtoul(rest + 1, &end, 10);
            if (*end == '\0' && index < setting->count) {
                *slot = index;
                return setting;
            }
        }
    }
    return NULL;
}

static int dax_settings_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                   void *cb_arg) {
    uint16_t slot;
    const struct dax_setting *setting = find_stored(name, &slot);
    if (!setting) {
        /* A setting a newer or older firmware had: left in flash, not loaded. */
        LOG_WRN("dax settings: ignoring %s/%s", DAX_SETTINGS_SUBTREE, name);
        return 0;
    }
    if (len > setting->size) {
        LOG_WRN("dax settings: %s/%s is %u bytes, more than %u; ignored", DAX_SETTINGS_SUBTREE,
                name, (unsigned)len, setting->size);
        return 0;
    }

    k_mutex_lock(&state_lock, K_FOREVER);
    ssize_t read = read_cb(cb_arg, saved_bytes(setting, slot), len);
    struct dax_setting_slot *state = &setting->slots[slot];
    if (read >= 0) {
        state->saved_set = true;
        state->saved_len = read;
        state->live_set = true;
        state->live_len = read;
        memcpy(live_bytes(setting, slot), saved_bytes(setting, slot), read);
    }
    k_mutex_unlock(&state_lock);
    return read < 0 ? read : 0;
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
