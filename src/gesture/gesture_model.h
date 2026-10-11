/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * What the gesture processor recognizes with: tuning, profiles, and gesture slots. It starts as
 * the devicetree's values and, with CONFIG_ZMK_DAX_GESTURE_RUNTIME, follows custom settings.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zmk/behavior.h>

#include "gesture_core.h"

#define DAX_GESTURE_NO_PROFILE 0xFF

struct dax_gesture_profile {
    uint32_t layers;
    uint32_t require_layers;
    bool suppress_movement;
};

struct dax_gesture_slot {
    /* Index of the profile the gesture belongs to, or DAX_GESTURE_NO_PROFILE when unused. */
    uint8_t profile;
    dax_gesture_pattern_t pattern;
    struct zmk_behavior_binding binding;
};

struct dax_gesture_model {
    struct dax_gesture_tuning tuning;
    struct dax_gesture_profile *profiles;
    size_t profiles_len;
    struct dax_gesture_slot *slots;
    size_t slots_len;
    /* profiles_len rows of slots_len patterns: a slot's pattern in its own profile's row and
     * zero elsewhere, so each row is a pattern set whose indexes are slot indexes. */
    dax_gesture_pattern_t *views;
};

static inline struct dax_gesture_set dax_gesture_model_set(const struct dax_gesture_model *model,
                                                            size_t profile) {
    return (struct dax_gesture_set){
        .patterns = &model->views[profile * model->slots_len],
        .count = model->slots_len,
    };
}

/* Call after changing profiles or slots. */
void dax_gesture_model_rebuild_views(struct dax_gesture_model *model);

#if IS_ENABLED(CONFIG_ZMK_DAX_GESTURE_RUNTIME)
/*
 * Keeps `model` in step with custom settings, starting from what it holds now (the devicetree
 * values). `lock` guards the model; `changed` runs, with the lock held, after every update.
 */
void dax_gesture_settings_attach(struct dax_gesture_model *model, struct k_mutex *lock,
                                 void (*changed)(void));
#endif
