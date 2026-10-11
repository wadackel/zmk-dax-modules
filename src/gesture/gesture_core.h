/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Stroke gesture recognition, free of any Zephyr or ZMK dependency so it can be
 * tested on the host. The caller feeds relative movement and the current time,
 * and arms a timer for `idle_deadline_ms` when one is set.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DAX_GESTURE_UP 1
#define DAX_GESTURE_DOWN 2
#define DAX_GESTURE_LEFT 3
#define DAX_GESTURE_RIGHT 4

#define DAX_GESTURE_MAX_STROKES 8
#define DAX_GESTURE_NO_MATCH (-1)
#define DAX_GESTURE_NO_DEADLINE INT64_MIN

/* Four bits per stroke, first stroke in the lowest bits; zero is "no pattern". */
typedef uint32_t dax_gesture_pattern_t;

struct dax_gesture_tuning {
    /* Movement, |x| + |y|, that makes one stroke. */
    uint32_t stroke_size;
    /* Single reports smaller than this are ignored. */
    uint32_t movement_threshold;
    /* Pause after which a recognized pattern fires. Zero disables the pause. */
    uint32_t idle_timeout_ms;
    /* Pause after which unfinished movement is forgotten. Zero disables it. */
    uint32_t partial_timeout_ms;
    /* Movement is ignored for this long after a gesture fired. */
    uint32_t cooldown_ms;
    /* Fire as soon as the strokes match a pattern no longer pattern starts with. */
    bool eager;
};

struct dax_gesture_set {
    const dax_gesture_pattern_t *patterns;
    size_t count;
};

struct dax_gesture_state {
    int32_t acc_x;
    int32_t acc_y;
    dax_gesture_pattern_t strokes;
    uint8_t stroke_count;
    uint8_t last_direction;
    bool moved;
    bool fired;
    int64_t last_movement_ms;
    int64_t last_fire_ms;
    /* When to call dax_gesture_idle_elapsed(), or DAX_GESTURE_NO_DEADLINE. */
    int64_t idle_deadline_ms;
};

void dax_gesture_init(struct dax_gesture_state *state);

/* Forgets the strokes so far. The cooldown after the last gesture keeps running. */
void dax_gesture_reset(struct dax_gesture_state *state);

/* Each returns the index of the pattern that fired, or DAX_GESTURE_NO_MATCH. */
int dax_gesture_feed(struct dax_gesture_state *state, const struct dax_gesture_tuning *tuning,
                     const struct dax_gesture_set *set, bool is_x, int32_t value, int64_t now_ms);
int dax_gesture_idle_elapsed(struct dax_gesture_state *state,
                             const struct dax_gesture_tuning *tuning,
                             const struct dax_gesture_set *set, int64_t now_ms);
/* Recognition is being switched off: settle what was drawn so far. */
int dax_gesture_end(struct dax_gesture_state *state, const struct dax_gesture_tuning *tuning,
                    const struct dax_gesture_set *set, int64_t now_ms);

/* True for one to DAX_GESTURE_MAX_STROKES strokes, each a direction, none repeating the last. */
bool dax_gesture_pattern_is_valid(dax_gesture_pattern_t pattern);
