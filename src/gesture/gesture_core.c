/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 */

#include "gesture_core.h"

#define STROKE_BITS 4
#define STROKE_MASK 0xFu
#define ACC_LIMIT (INT32_MAX / 2)

static uint32_t magnitude(int32_t value) { return value < 0 ? (uint32_t)(-value) : (uint32_t)value; }

static dax_gesture_pattern_t prefix_mask(uint8_t stroke_count) {
    return stroke_count >= DAX_GESTURE_MAX_STROKES ? UINT32_MAX
                                                   : (1u << (stroke_count * STROKE_BITS)) - 1;
}

static int find_exact(const struct dax_gesture_set *set, dax_gesture_pattern_t strokes) {
    for (size_t i = 0; i < set->count; i++) {
        if (set->patterns[i] != 0 && set->patterns[i] == strokes) {
            return (int)i;
        }
    }
    return DAX_GESTURE_NO_MATCH;
}

/* A pattern that starts with these strokes; `longer_only` skips the exact match. */
static bool any_starts_with(const struct dax_gesture_set *set, dax_gesture_pattern_t strokes,
                            uint8_t stroke_count, bool longer_only) {
    dax_gesture_pattern_t mask = prefix_mask(stroke_count);

    for (size_t i = 0; i < set->count; i++) {
        dax_gesture_pattern_t pattern = set->patterns[i];
        if (pattern == 0 || (pattern & mask) != strokes) {
            continue;
        }
        if (!longer_only || pattern != strokes) {
            return true;
        }
    }
    return false;
}

void dax_gesture_reset(struct dax_gesture_state *state) {
    state->acc_x = 0;
    state->acc_y = 0;
    state->strokes = 0;
    state->stroke_count = 0;
    state->last_direction = 0;
    state->moved = false;
    state->idle_deadline_ms = DAX_GESTURE_NO_DEADLINE;
}

void dax_gesture_init(struct dax_gesture_state *state) {
    dax_gesture_reset(state);
    state->fired = false;
    state->last_movement_ms = 0;
    state->last_fire_ms = 0;
}

static bool cooling_down(const struct dax_gesture_state *state,
                         const struct dax_gesture_tuning *tuning, int64_t now_ms) {
    return state->fired && now_ms - state->last_fire_ms < (int64_t)tuning->cooldown_ms;
}

/* `settle` means no more strokes can follow, so an unmatched drawing is dropped. */
static int match(struct dax_gesture_state *state, const struct dax_gesture_tuning *tuning,
                 const struct dax_gesture_set *set, int64_t now_ms, bool settle) {
    int index = state->stroke_count > 0 ? find_exact(set, state->strokes) : DAX_GESTURE_NO_MATCH;

    if (index == DAX_GESTURE_NO_MATCH) {
        if (settle) {
            dax_gesture_reset(state);
        }
        return DAX_GESTURE_NO_MATCH;
    }

    if (cooling_down(state, tuning, now_ms)) {
        return DAX_GESTURE_NO_MATCH;
    }

    /* A longer pattern may still be coming: wait for a pause before firing the short one. */
    if (tuning->eager && !settle && tuning->idle_timeout_ms > 0 &&
        any_starts_with(set, state->strokes, state->stroke_count, true)) {
        state->idle_deadline_ms = now_ms + tuning->idle_timeout_ms;
        return DAX_GESTURE_NO_MATCH;
    }

    state->fired = true;
    state->last_fire_ms = now_ms;
    dax_gesture_reset(state);
    return index;
}

static void accumulate(int32_t *acc, int32_t value) {
    int64_t next = (int64_t)*acc + value;

    if (next > ACC_LIMIT) {
        next = ACC_LIMIT;
    } else if (next < -ACC_LIMIT) {
        next = -ACC_LIMIT;
    }
    *acc = (int32_t)next;
}

static uint8_t dominant_direction(int32_t x, int32_t y) {
    if (magnitude(x) > magnitude(y)) {
        return x > 0 ? DAX_GESTURE_RIGHT : DAX_GESTURE_LEFT;
    }
    return y > 0 ? DAX_GESTURE_DOWN : DAX_GESTURE_UP;
}

static void add_stroke(struct dax_gesture_state *state, const struct dax_gesture_tuning *tuning,
                       const struct dax_gesture_set *set, int64_t now_ms) {
    uint8_t direction = dominant_direction(state->acc_x, state->acc_y);

    /* A long stroke is still one stroke. */
    if (direction == state->last_direction) {
        state->acc_x = 0;
        state->acc_y = 0;
        return;
    }

    if (state->stroke_count >= DAX_GESTURE_MAX_STROKES) {
        dax_gesture_reset(state);
        return;
    }

    dax_gesture_pattern_t strokes =
        state->strokes | ((dax_gesture_pattern_t)direction << (state->stroke_count * STROKE_BITS));
    uint8_t stroke_count = state->stroke_count + 1;

    if (!any_starts_with(set, strokes, stroke_count, false)) {
        dax_gesture_reset(state);
        return;
    }

    if (!tuning->eager && tuning->idle_timeout_ms > 0 && state->stroke_count == 0) {
        state->idle_deadline_ms = now_ms + tuning->idle_timeout_ms;
    }

    state->strokes = strokes;
    state->stroke_count = stroke_count;
    state->last_direction = direction;
    state->acc_x = 0;
    state->acc_y = 0;
}

int dax_gesture_feed(struct dax_gesture_state *state, const struct dax_gesture_tuning *tuning,
                     const struct dax_gesture_set *set, bool is_x, int32_t value, int64_t now_ms) {
    if (magnitude(value) < tuning->movement_threshold) {
        return DAX_GESTURE_NO_MATCH;
    }

    if (!cooling_down(state, tuning, now_ms)) {
        if (tuning->partial_timeout_ms > 0 && state->moved &&
            now_ms - state->last_movement_ms > (int64_t)tuning->partial_timeout_ms) {
            dax_gesture_reset(state);
        }

        accumulate(is_x ? &state->acc_x : &state->acc_y, value);
        state->moved = true;
        state->last_movement_ms = now_ms;

        if (!tuning->eager && tuning->idle_timeout_ms > 0 && state->stroke_count > 0) {
            state->idle_deadline_ms = now_ms + tuning->idle_timeout_ms;
        }

        if (magnitude(state->acc_x) + magnitude(state->acc_y) >= tuning->stroke_size) {
            add_stroke(state, tuning, set, now_ms);
        }
    }

    return tuning->eager ? match(state, tuning, set, now_ms, false) : DAX_GESTURE_NO_MATCH;
}

int dax_gesture_idle_elapsed(struct dax_gesture_state *state,
                             const struct dax_gesture_tuning *tuning,
                             const struct dax_gesture_set *set, int64_t now_ms) {
    state->idle_deadline_ms = DAX_GESTURE_NO_DEADLINE;
    if (state->stroke_count == 0) {
        return DAX_GESTURE_NO_MATCH;
    }
    return match(state, tuning, set, now_ms, true);
}

int dax_gesture_end(struct dax_gesture_state *state, const struct dax_gesture_tuning *tuning,
                    const struct dax_gesture_set *set, int64_t now_ms) {
    int index = match(state, tuning, set, now_ms, true);

    dax_gesture_reset(state);
    return index;
}

bool dax_gesture_capture_feed(struct dax_gesture_state *state,
                              const struct dax_gesture_tuning *tuning, bool is_x, int32_t value,
                              int64_t now_ms, uint32_t settle_ms) {
    if (magnitude(value) < tuning->movement_threshold) {
        return false;
    }

    accumulate(is_x ? &state->acc_x : &state->acc_y, value);
    state->moved = true;
    state->last_movement_ms = now_ms;
    state->idle_deadline_ms = now_ms + settle_ms;

    if (magnitude(state->acc_x) + magnitude(state->acc_y) < tuning->stroke_size) {
        return false;
    }

    uint8_t direction = dominant_direction(state->acc_x, state->acc_y);

    state->acc_x = 0;
    state->acc_y = 0;
    if (direction == state->last_direction || state->stroke_count >= DAX_GESTURE_MAX_STROKES) {
        return false;
    }

    state->strokes |= (dax_gesture_pattern_t)direction << (state->stroke_count * STROKE_BITS);
    state->stroke_count++;
    state->last_direction = direction;
    return true;
}

bool dax_gesture_pattern_is_valid(dax_gesture_pattern_t pattern) {
    uint8_t previous = 0;
    bool ended = false;

    if (pattern == 0) {
        return false;
    }
    for (int i = 0; i < DAX_GESTURE_MAX_STROKES; i++) {
        uint8_t stroke = (pattern >> (i * STROKE_BITS)) & STROKE_MASK;

        if (stroke == 0) {
            ended = true;
            continue;
        }
        if (ended || stroke > DAX_GESTURE_RIGHT || stroke == previous) {
            return false;
        }
        previous = stroke;
    }
    return true;
}
