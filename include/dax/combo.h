/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 *
 * Adapted from cormoran/zmk-feature-runtime-combo
 * (include/cormoran/runtime_combo/runtime_combo.h at d9490e7) under the dax_combo names.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/behavior.h>

#define DAX_COMBO_SUBSYSTEM_ID "dax_combo"
/* The most bytes a combo's name holds, without a terminator. */
#define DAX_COMBO_NAME_MAX 63

/* Tri-state override for the global slow-release setting. */
enum dax_combo_slow_release_override {
    DAX_COMBO_SLOW_RELEASE_INHERIT = 0,
    DAX_COMBO_SLOW_RELEASE_ON = 1,
    DAX_COMBO_SLOW_RELEASE_OFF = 2,
};

struct dax_combo_config {
    bool enabled;
    uint8_t key_position_len;
    uint32_t layer_mask;
    /* behavior_dev is NULL when behavior_id names no behavior in this build. */
    struct zmk_behavior_binding behavior;
    zmk_behavior_local_id_t behavior_id;
    /* 0 means inherit the global timeout. */
    uint16_t timeout_ms;
    /* 0 means inherit the global require-prior-idle setting. */
    uint16_t require_prior_idle_ms;
    enum dax_combo_slow_release_override slow_release_override;
    int32_t key_positions[CONFIG_ZMK_DAX_COMBO_MAX_KEYS_PER_COMBO];
};

struct dax_combo_global_settings {
    uint16_t timeout_ms;
    bool slow_release;
    uint32_t max_combo;
    /* 0 disables the require-prior-idle guard. */
    uint16_t require_prior_idle_ms;
};

/*
 * A slot holds what was written to it, or else its devicetree default. -ENOENT: neither.
 * -EBADMSG: a stored record this firmware cannot read; the slot acts as empty until it is
 * written or reset.
 */
int dax_combo_read(uint32_t index, struct dax_combo_config *combo);
int dax_combo_read_name(uint32_t index, char *name, size_t name_size);
int dax_combo_read_global_settings(struct dax_combo_global_settings *settings);
uint32_t dax_combo_max_count(void);

/* Every write changes RAM only: dax_combo_save keeps it, dax_combo_discard drops it. */
int dax_combo_write(uint32_t index, const struct dax_combo_config *combo);
/* An empty name shows the default's name again, where the slot has a default. */
int dax_combo_write_name(uint32_t index, const char *name);
int dax_combo_write_timeout_ms(uint16_t timeout_ms);
int dax_combo_write_slow_release(bool slow_release);
int dax_combo_write_require_prior_idle_ms(uint16_t require_prior_idle_ms);
/* Keeps the slot and turns the combo off, so a default underneath stays hidden. */
int dax_combo_delete(uint32_t index);
/* Forgets what was written to the slot, so its devicetree default (or nothing) shows again. */
int dax_combo_reset(uint32_t index);
int dax_combo_save(uint32_t *written);
int dax_combo_discard(uint32_t *changed);

uint32_t dax_combo_default_count(void);
/* The slot's devicetree default; -ENOENT if it has none. */
int dax_combo_read_default(uint32_t index, struct dax_combo_config *combo);
bool dax_combo_has_default(uint32_t index);
/* True if something was written to the slot (including a disabled default). */
bool dax_combo_has_override(uint32_t index);

/* Rebuild the matching table from the settings above. Runs on the system work queue, and waits
 * until no combo key is held. */
void dax_combo_engine_request_rebuild(void);
