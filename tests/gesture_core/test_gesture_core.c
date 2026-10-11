/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Host tests for the gesture recognizer. Build and run with tests/gesture_core/run.sh.
 */

#include <stdio.h>
#include <string.h>

#include "../../src/gesture/gesture_core.h"

#define U DAX_GESTURE_UP
#define D DAX_GESTURE_DOWN
#define L DAX_GESTURE_LEFT
#define R DAX_GESTURE_RIGHT
#define P1(a) ((dax_gesture_pattern_t)(a))
#define P2(a, b) (P1(a) | ((dax_gesture_pattern_t)(b) << 4))
#define P3(a, b, c) (P2(a, b) | ((dax_gesture_pattern_t)(c) << 8))
#define NONE DAX_GESTURE_NO_MATCH

static int failures;
static const char *current_test;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            failures++;                                                                            \
            printf("FAIL %s:%d: %s\n  %s\n", current_test, __LINE__, #condition, __FILE__);        \
        }                                                                                          \
    } while (0)

/* The dax6 defaults: eager, 300 per stroke, 150 ms pause, 200 ms cooldown, 400 ms stale. */
static const struct dax_gesture_tuning eager = {
    .stroke_size = 300,
    .idle_timeout_ms = 150,
    .partial_timeout_ms = 400,
    .cooldown_ms = 200,
    .eager = true,
};

static struct dax_gesture_tuning patient(void) {
    struct dax_gesture_tuning tuning = eager;
    tuning.eager = false;
    return tuning;
}

static const dax_gesture_pattern_t four_way[] = {P1(L), P1(R), P1(U), P1(D)};
static const struct dax_gesture_set four_way_set = {four_way, 4};

static const dax_gesture_pattern_t prefixed[] = {P1(D), P2(D, R), P3(D, R, U)};
static const struct dax_gesture_set prefixed_set = {prefixed, 3};

struct rig {
    struct dax_gesture_state state;
    struct dax_gesture_tuning tuning;
    const struct dax_gesture_set *set;
    int64_t now;
};

static struct rig rig_with(struct dax_gesture_tuning tuning, const struct dax_gesture_set *set) {
    struct rig rig = {.tuning = tuning, .set = set, .now = 1000};
    dax_gesture_init(&rig.state);
    return rig;
}

static int move_x(struct rig *rig, int32_t value) {
    return dax_gesture_feed(&rig->state, &rig->tuning, rig->set, true, value, rig->now);
}

static int move_y(struct rig *rig, int32_t value) {
    return dax_gesture_feed(&rig->state, &rig->tuning, rig->set, false, value, rig->now);
}

static int pause_until_deadline(struct rig *rig) {
    rig->now = rig->state.idle_deadline_ms;
    return dax_gesture_idle_elapsed(&rig->state, &rig->tuning, rig->set, rig->now);
}

static void test_fires_one_stroke_as_soon_as_it_is_long_enough(void) {
    struct rig rig = rig_with(eager, &four_way_set);

    CHECK(move_x(&rig, -299) == NONE);
    CHECK(move_x(&rig, -1) == 0);
    CHECK(rig.state.idle_deadline_ms == DAX_GESTURE_NO_DEADLINE);
}

static void test_picks_the_axis_that_moved_more(void) {
    struct rig rig = rig_with(eager, &four_way_set);

    CHECK(move_x(&rig, 100) == NONE);
    CHECK(move_y(&rig, -200) == 2);

    rig = rig_with(eager, &four_way_set);
    CHECK(move_y(&rig, 100) == NONE);
    CHECK(move_x(&rig, 200) == 1);

    /* A tie goes to the vertical axis. */
    rig = rig_with(eager, &four_way_set);
    CHECK(move_x(&rig, 150) == NONE);
    CHECK(move_y(&rig, 150) == 3);
}

static void test_ignores_movement_during_the_cooldown_then_fires_again(void) {
    struct rig rig = rig_with(eager, &four_way_set);

    CHECK(move_x(&rig, 300) == 1);
    rig.now += 199;
    CHECK(move_x(&rig, 300) == NONE);
    rig.now += 1;
    CHECK(move_x(&rig, 299) == NONE);
    CHECK(move_x(&rig, 1) == 1);
}

static void test_treats_one_long_stroke_as_a_single_stroke(void) {
    struct rig rig = rig_with(patient(), &prefixed_set);

    CHECK(move_y(&rig, 300) == NONE);
    CHECK(move_y(&rig, 300) == NONE);
    CHECK(move_y(&rig, 300) == NONE);
    CHECK(rig.state.stroke_count == 1);
    CHECK(pause_until_deadline(&rig) == 0);
}

static void test_eager_waits_for_a_pause_when_a_longer_pattern_could_follow(void) {
    struct rig rig = rig_with(eager, &prefixed_set);

    CHECK(move_y(&rig, 300) == NONE);
    CHECK(rig.state.idle_deadline_ms == rig.now + 150);
    CHECK(pause_until_deadline(&rig) == 0);
}

static void test_eager_fires_the_longest_pattern_at_once(void) {
    struct rig rig = rig_with(eager, &prefixed_set);

    CHECK(move_y(&rig, 300) == NONE);
    rig.now += 50;
    CHECK(move_x(&rig, 300) == NONE);
    rig.now += 50;
    CHECK(move_y(&rig, -300) == 2);
    CHECK(rig.state.idle_deadline_ms == DAX_GESTURE_NO_DEADLINE);
}

static void test_eager_pause_fires_the_middle_pattern(void) {
    struct rig rig = rig_with(eager, &prefixed_set);

    CHECK(move_y(&rig, 300) == NONE);
    rig.now += 50;
    CHECK(move_x(&rig, 300) == NONE);
    CHECK(pause_until_deadline(&rig) == 1);
}

static void test_patient_mode_fires_on_a_pause(void) {
    struct rig rig = rig_with(patient(), &four_way_set);

    CHECK(move_x(&rig, 300) == NONE);
    CHECK(rig.state.idle_deadline_ms == rig.now + 150);
    rig.now += 100;
    CHECK(move_x(&rig, 10) == NONE);
    CHECK(rig.state.idle_deadline_ms == rig.now + 150);
    CHECK(pause_until_deadline(&rig) == 1);
}

static void test_patient_mode_fires_when_recognition_ends(void) {
    struct rig rig = rig_with(patient(), &four_way_set);

    CHECK(move_y(&rig, -300) == NONE);
    CHECK(dax_gesture_end(&rig.state, &rig.tuning, rig.set, rig.now) == 2);
    CHECK(rig.state.stroke_count == 0);
}

static void test_ending_without_a_complete_pattern_fires_nothing(void) {
    struct rig rig = rig_with(patient(), &four_way_set);

    CHECK(move_y(&rig, -200) == NONE);
    CHECK(dax_gesture_end(&rig.state, &rig.tuning, rig.set, rig.now) == NONE);
    CHECK(rig.state.acc_y == 0);
}

static void test_a_stroke_no_pattern_continues_with_starts_over(void) {
    struct rig rig = rig_with(eager, &prefixed_set);

    CHECK(move_y(&rig, 300) == NONE);
    CHECK(move_x(&rig, -300) == NONE);
    CHECK(rig.state.stroke_count == 0);
    CHECK(rig.state.idle_deadline_ms == DAX_GESTURE_NO_DEADLINE);
}

static void test_forgets_unfinished_movement_after_a_long_pause(void) {
    struct rig rig = rig_with(eager, &four_way_set);

    CHECK(move_x(&rig, 250) == NONE);
    rig.now += 401;
    CHECK(move_y(&rig, 100) == NONE);
    CHECK(rig.state.acc_x == 0);
    CHECK(move_y(&rig, 200) == 3);
}

static void test_keeps_unfinished_movement_across_a_short_pause(void) {
    struct rig rig = rig_with(eager, &four_way_set);

    CHECK(move_x(&rig, 250) == NONE);
    rig.now += 400;
    CHECK(move_x(&rig, 50) == 1);
}

static void test_ignores_reports_below_the_movement_threshold(void) {
    struct dax_gesture_tuning tuning = eager;
    tuning.movement_threshold = 5;
    struct rig rig = rig_with(tuning, &four_way_set);

    for (int i = 0; i < 200; i++) {
        CHECK(move_x(&rig, 4) == NONE);
    }
    CHECK(rig.state.acc_x == 0);
    CHECK(move_x(&rig, 300) == 1);
}

static void test_skips_unused_slots(void) {
    static const dax_gesture_pattern_t sparse[] = {0, P1(U), 0, P1(D)};
    static const struct dax_gesture_set sparse_set = {sparse, 4};
    struct rig rig = rig_with(eager, &sparse_set);

    CHECK(move_y(&rig, 300) == 3);
    rig.now += 200;
    CHECK(move_x(&rig, 300) == NONE);
    CHECK(rig.state.stroke_count == 0);
}

static void test_an_empty_set_never_fires(void) {
    static const struct dax_gesture_set empty = {NULL, 0};
    struct rig rig = rig_with(eager, &empty);

    CHECK(move_x(&rig, 1000) == NONE);
    CHECK(dax_gesture_end(&rig.state, &rig.tuning, rig.set, rig.now) == NONE);
}

static void test_huge_movement_does_not_overflow(void) {
    struct rig rig = rig_with(patient(), &four_way_set);

    CHECK(move_x(&rig, INT32_MAX) == NONE);
    CHECK(move_x(&rig, INT32_MAX) == NONE);
    CHECK(pause_until_deadline(&rig) == 1);
}

static void test_capture_records_strokes_no_pattern_knows(void) {
    struct rig rig = rig_with(eager, &four_way_set);
    struct dax_gesture_state *state = &rig.state;

    CHECK(!dax_gesture_capture_feed(state, &rig.tuning, false, 200, rig.now, 600));
    CHECK(dax_gesture_capture_feed(state, &rig.tuning, false, 100, rig.now, 600));
    CHECK(!dax_gesture_capture_feed(state, &rig.tuning, false, 400, rig.now, 600));
    rig.now += 100;
    CHECK(dax_gesture_capture_feed(state, &rig.tuning, true, -300, rig.now, 600));
    CHECK(dax_gesture_capture_feed(state, &rig.tuning, false, 300, rig.now, 600));
    CHECK(state->strokes == P3(D, L, D));
    CHECK(state->stroke_count == 3);
    CHECK(state->idle_deadline_ms == rig.now + 600);
}

static void test_capture_stops_at_the_longest_pattern(void) {
    struct rig rig = rig_with(eager, &four_way_set);

    for (int i = 0; i < 12; i++) {
        dax_gesture_capture_feed(&rig.state, &rig.tuning, i % 2 == 0, 300, rig.now, 600);
    }
    CHECK(rig.state.stroke_count == DAX_GESTURE_MAX_STROKES);
    CHECK(dax_gesture_pattern_is_valid(rig.state.strokes));
}

static void test_validates_patterns(void) {
    CHECK(dax_gesture_pattern_is_valid(P1(U)));
    CHECK(dax_gesture_pattern_is_valid(P3(D, R, U)));
    CHECK(dax_gesture_pattern_is_valid(0x12121212u));
    CHECK(!dax_gesture_pattern_is_valid(0));
    CHECK(!dax_gesture_pattern_is_valid(P1(5)));
    CHECK(!dax_gesture_pattern_is_valid(P2(U, U)));
    CHECK(!dax_gesture_pattern_is_valid(P1(U) | (P1(D) << 8)));
}

#define RUN(test)                                                                                  \
    do {                                                                                           \
        current_test = #test;                                                                      \
        test();                                                                                    \
    } while (0)

int main(void) {
    RUN(test_fires_one_stroke_as_soon_as_it_is_long_enough);
    RUN(test_picks_the_axis_that_moved_more);
    RUN(test_ignores_movement_during_the_cooldown_then_fires_again);
    RUN(test_treats_one_long_stroke_as_a_single_stroke);
    RUN(test_eager_waits_for_a_pause_when_a_longer_pattern_could_follow);
    RUN(test_eager_fires_the_longest_pattern_at_once);
    RUN(test_eager_pause_fires_the_middle_pattern);
    RUN(test_patient_mode_fires_on_a_pause);
    RUN(test_patient_mode_fires_when_recognition_ends);
    RUN(test_ending_without_a_complete_pattern_fires_nothing);
    RUN(test_a_stroke_no_pattern_continues_with_starts_over);
    RUN(test_forgets_unfinished_movement_after_a_long_pause);
    RUN(test_keeps_unfinished_movement_across_a_short_pause);
    RUN(test_ignores_reports_below_the_movement_threshold);
    RUN(test_skips_unused_slots);
    RUN(test_an_empty_set_never_fires);
    RUN(test_huge_movement_does_not_overflow);
    RUN(test_capture_records_strokes_no_pattern_knows);
    RUN(test_capture_stops_at_the_longest_pattern);
    RUN(test_validates_patterns);

    if (failures > 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("gesture_core: all tests passed\n");
    return 0;
}
