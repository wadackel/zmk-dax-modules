/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Plays macros one after another, each a step at a time from its own work item. ZMK's behavior
 * queue is not used: a macro put on it whole overflows it (64 entries by default) partway through
 * a long run of keys, which drops the rest and can leave a key pressed.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dax/macro.h>
#include <zmk/behavior.h>

#include "macro_body.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct play {
    uint32_t slot;
    struct zmk_behavior_binding_event event;
};

K_MSGQ_DEFINE(dax_macro_plays, sizeof(struct play), CONFIG_ZMK_DAX_MACRO_PLAY_QUEUE, 4);

static void play_next(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(play_work, play_next);

/* Touched only by play_work. The body is a copy taken when the play starts, so an edit made
 * while it plays does not change it. */
static struct {
    bool active;
    struct zmk_behavior_binding_event event;
    uint8_t body[CONFIG_ZMK_DAX_MACRO_MAX_BYTES];
    size_t len;
    size_t offset;
    /* The key tap sequence being played, and the next of its keys. */
    struct dax_macro_step keys;
    uint8_t next_key;
    /* A tap whose key is let go of when play_work runs next. */
    bool tapping;
    struct zmk_behavior_binding tapped;
} player;

static void invoke(const struct zmk_behavior_binding *binding, bool pressed) {
    struct zmk_behavior_binding_event event = player.event;
    event.timestamp = k_uptime_get();
    zmk_behavior_invoke_binding(binding, event, pressed);
}

static bool start(void) {
    struct play play;
    while (k_msgq_get(&dax_macro_plays, &play, K_NO_WAIT) == 0) {
        int len = dax_macro_read(play.slot, NULL, 0, player.body, sizeof(player.body));
        if (len == -ENOENT) {
            LOG_DBG("dax macro: slot %u shows no macro to play", play.slot);
            continue;
        }
        /* Checked whole before the first step, so a macro is never left half played. */
        int ret = len < 0 ? len : dax_macro_body_check_bindings(player.body, len);
        if (ret < 0) {
            LOG_WRN("dax macro: slot %u cannot be played (%d)", play.slot, ret);
            continue;
        }
        player.active = true;
        player.event = play.event;
        player.len = len;
        player.offset = 0;
        player.keys.key_count = 0;
        player.next_key = 0;
        return true;
    }
    return false;
}

static void tap(const struct zmk_behavior_binding *binding) {
    player.tapped = *binding;
    player.tapping = true;
    invoke(binding, true);
}

/* Runs the next item of the macro and returns how long to wait after it, or -ENOENT when the
 * macro is over. */
static int64_t play_item(void) {
    struct zmk_behavior_binding binding;
    if (player.next_key < player.keys.key_count) {
        dax_macro_key_binding(player.keys.keys[player.next_key++], &binding);
        tap(&binding);
        return dax_macro_tap_ms();
    }

    struct dax_macro_step step;
    if (dax_macro_next_step(player.body, player.len, &player.offset, &step) < 0) {
        return -ENOENT;
    }
    switch (step.kind) {
    case DAX_MACRO_STEP_DOWN:
    case DAX_MACRO_STEP_UP:
        dax_macro_step_binding(&step, &binding);
        invoke(&binding, step.kind == DAX_MACRO_STEP_DOWN);
        return 0;
    case DAX_MACRO_STEP_TAP:
        dax_macro_step_binding(&step, &binding);
        tap(&binding);
        return dax_macro_tap_ms();
    case DAX_MACRO_STEP_DELAY:
        return step.delay_ms;
    default:
        player.keys = step;
        player.next_key = 0;
        return 0;
    }
}

static void play_next(struct k_work *work) {
    ARG_UNUSED(work);
    for (;;) {
        if (player.tapping) {
            player.tapping = false;
            invoke(&player.tapped, false);
        }
        if (!player.active && !start()) {
            return;
        }
        int64_t wait = play_item();
        if (wait < 0) {
            player.active = false;
        } else if (wait > 0) {
            /* Reschedule: a play queued from a step of this macro has already submitted the
             * work, and k_work_schedule would then leave the wait out. */
            k_work_reschedule(&play_work, K_MSEC(wait));
            return;
        }
    }
}

int dax_macro_play(uint32_t slot, const struct zmk_behavior_binding_event *event) {
    if (slot >= CONFIG_ZMK_DAX_MACRO_COUNT) {
        return -ENOENT;
    }
    struct play play = {.slot = slot, .event = *event};
    if (k_msgq_put(&dax_macro_plays, &play, K_NO_WAIT) < 0) {
        LOG_WRN("dax macro: the play queue is full, dropped slot %u", slot);
        return -ENOSPC;
    }
    /* Not k_work_reschedule: that would cut short a wait already pending. */
    k_work_schedule(&play_work, K_NO_WAIT);
    return 0;
}
