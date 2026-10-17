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
#define DAX_COMBO_COMBOS_KEY "combos"
#define DAX_COMBO_NAMES_KEY "names"
#define DAX_COMBO_BEHAVIORS_KEY "behaviors"
#define DAX_COMBO_TIMEOUT_MS_KEY "timeout_ms"
#define DAX_COMBO_SLOW_RELEASE_KEY "slow_release"
#define DAX_COMBO_REQUIRE_PRIOR_IDLE_MS_KEY "require_prior_idle_ms"

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
    struct zmk_behavior_binding behavior;
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

int dax_combo_read(uint32_t index, struct dax_combo_config *combo);
int dax_combo_read_name(uint32_t index, char *name, size_t name_size);
int dax_combo_read_global_settings(struct dax_combo_global_settings *settings);
uint32_t dax_combo_count(void);
uint32_t dax_combo_max_count(void);
int dax_combo_write(uint32_t index, const struct dax_combo_config *combo, bool persist);
int dax_combo_write_name(uint32_t index, const char *name, bool persist);
int dax_combo_write_timeout_ms(uint16_t timeout_ms, bool persist);
int dax_combo_write_slow_release(bool slow_release, bool persist);
int dax_combo_write_require_prior_idle_ms(uint16_t require_prior_idle_ms, bool persist);
int dax_combo_delete(uint32_t index, bool persist);

/* Number of combo slots with a compile-time default (0 if the
 * dax,runtime-combo-defaults devicetree node is absent). */
uint32_t dax_combo_default_count(void);
/* Read the compile-time default for a slot. Returns -ENOENT if the slot has
 * no default, regardless of any stored runtime value. */
int dax_combo_read_default(uint32_t index, struct dax_combo_config *combo);
/* True if the slot has a compile-time default. */
bool dax_combo_has_default(uint32_t index);
/* True if the slot has a stored runtime value (set or explicitly deleted),
 * regardless of whether it also has a compile-time default. */
bool dax_combo_has_override(uint32_t index);
/* Erase a stored runtime override, restoring the compile-time default (or an
 * empty slot, if there is none). */
int dax_combo_reset(uint32_t index);

/* Rebuild the matching table from the settings above. Runs on the system work queue, and waits
 * until no combo key is held. */
void dax_combo_engine_request_rebuild(void);
