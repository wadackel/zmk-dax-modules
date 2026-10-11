/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 */

#include "gesture_model.h"

void dax_gesture_model_rebuild_views(struct dax_gesture_model *model) {
    for (size_t profile = 0; profile < model->profiles_len; profile++) {
        for (size_t slot = 0; slot < model->slots_len; slot++) {
            const struct dax_gesture_slot *entry = &model->slots[slot];
            bool usable = entry->profile == profile && entry->binding.behavior_dev != NULL &&
                          dax_gesture_pattern_is_valid(entry->pattern);

            model->views[profile * model->slots_len + slot] = usable ? entry->pattern : 0;
        }
    }
}
