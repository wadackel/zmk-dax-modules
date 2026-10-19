/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Macros in fixed slots, the number a `&dax_macro <slot>` binding plays. A slot shows what was
 * written to it, or else its devicetree default, or nothing. Every change, creating, renaming and
 * deleting a macro included, stays in RAM until dax_macro_save; dax_macro_discard puts it back.
 *
 * A slot that is out of range, or that shows nothing where a macro is needed, is -ENOENT. A step
 * or a name that is not well formed is -EINVAL, a name or a body over its limit -EMSGSIZE, a name
 * another slot shows -EEXIST, a create with no slot to take -ENOSPC, and a delete of a macro that
 * comes with the devicetree -EPERM.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/behavior.h>

#define DAX_MACRO_SUBSYSTEM_ID "dax_macro"
/* The most keys one key tap sequence step holds, as the dax_macro RPC carries it. */
#define DAX_MACRO_KEY_TAP_SEQUENCE_MAX 64

/* The values are the step's opcode in a stored body. */
enum dax_macro_step_kind {
    DAX_MACRO_STEP_DOWN = 1,
    DAX_MACRO_STEP_UP = 2,
    DAX_MACRO_STEP_TAP = 3,
    DAX_MACRO_STEP_DELAY = 4,
    DAX_MACRO_STEP_KEY_TAP_SEQUENCE = 5,
};

struct dax_macro_step {
    enum dax_macro_step_kind kind;
    /* DOWN, UP and TAP. */
    zmk_behavior_local_id_t behavior_id;
    uint32_t param1;
    uint32_t param2;
    /* DELAY. */
    uint32_t delay_ms;
    /* KEY_TAP_SEQUENCE: one byte a key, the low seven bits a keyboard usage from 0x04 to 0x38, bit
     * 7 adds left shift. */
    uint8_t key_count;
    uint8_t keys[DAX_MACRO_KEY_TAP_SEQUENCE_MAX];
};

enum dax_macro_source {
    /* Nothing written and no default, or a record this firmware cannot read. */
    DAX_MACRO_SOURCE_EMPTY,
    DAX_MACRO_SOURCE_DEFAULT,
    /* A devicetree default exists, and a macro written at runtime takes its place. */
    DAX_MACRO_SOURCE_OVERRIDDEN,
    DAX_MACRO_SOURCE_RUNTIME,
};

/* The macro the slot shows: its name, NUL-terminated (name_size at least
 * CONFIG_ZMK_DAX_MACRO_NAME_MAX + 1), and its encoded body, whose length is returned. Either
 * buffer may be NULL. */
int dax_macro_read(uint32_t slot, char *name, size_t name_size, uint8_t *body, size_t body_size);
enum dax_macro_source dax_macro_source(uint32_t slot);
/* Whether the slot differs from what was last saved. A default nobody edited is not unsaved. */
bool dax_macro_is_unsaved(uint32_t slot);
uint16_t dax_macro_tap_ms(void);

/* The step at *offset of an encoded body, which advances past it; -ENOENT after the last one.
 * *offset starts at 0. Only the form is checked, not that a behavior exists. */
int dax_macro_next_step(const uint8_t *body, size_t len, size_t *offset,
                        struct dax_macro_step *step);

/* An empty macro in the first slot that shows nothing and has no devicetree default. */
int dax_macro_create(const char *name, uint32_t *slot);
int dax_macro_rename(uint32_t slot, const char *name);
/* Removes a macro written at runtime. One that comes with the devicetree cannot be deleted; a
 * reset puts it back instead. */
int dax_macro_delete(uint32_t slot);
/* Forgets what was written to the slot, so its devicetree default, or nothing, shows again. */
int dax_macro_reset(uint32_t slot);
int dax_macro_set_step(uint32_t slot, uint32_t index, const struct dax_macro_step *step);
int dax_macro_append_step(uint32_t slot, const struct dax_macro_step *step);
/* Keeps the first `count` steps; a count above the macro's own is -EINVAL. */
int dax_macro_set_step_count(uint32_t slot, uint32_t count);
int dax_macro_write_tap_ms(uint16_t tap_ms);
int dax_macro_save(uint32_t *written);
int dax_macro_discard(uint32_t *changed);

/* Queues a play of what the slot shows when its turn comes; plays run one after another.
 * -ENOSPC: the queue was full and the play is dropped. */
int dax_macro_play(uint32_t slot, const struct zmk_behavior_binding_event *event);
