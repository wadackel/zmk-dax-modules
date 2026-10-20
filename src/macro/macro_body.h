/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 *
 * Adapted from cormoran/zmk-feature-runtime-macro (src/runtime_macro.c at 3853096): the encoded
 * body of a macro, format 1, under the dax_macro names.
 *
 *   0   version (1)
 *   1.. steps, each an opcode (enum dax_macro_step_kind) and its arguments:
 *       DOWN, UP, TAP       behavior local id, param1, param2, each a varint
 *       DELAY               milliseconds, a varint
 *       KEY_TAP_SEQUENCE    key count n (a varint), then n packed keys
 *
 * A varint is seven bits a byte, low bits first, bit 7 set on every byte but the last.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <dax/macro.h>
#include <zmk/behavior.h>

#define DAX_MACRO_BODY_VERSION 1

/* 0 when the body is well formed. */
int dax_macro_body_check(const uint8_t *body, size_t len);
/* Also checks that every behavior exists and accepts its parameters, which only the build that
 * plays a macro can say. */
int dax_macro_body_check_bindings(const uint8_t *body, size_t len);
/* Appends the step at *offset; -EMSGSIZE when it does not fit in `size`. */
int dax_macro_body_put_step(uint8_t *body, size_t size, size_t *offset,
                            const struct dax_macro_step *step);
/* The binding a DOWN, UP or TAP step invokes: -ENODEV for a behavior this build lacks, the error
 * of zmk_behavior_validate_binding for parameters it refuses. */
int dax_macro_step_binding(const struct dax_macro_step *step,
                           struct zmk_behavior_binding *binding);
/* The &kp binding of one packed key. */
int dax_macro_key_binding(uint8_t packed, struct zmk_behavior_binding *binding);
/* -EINVAL for a keycode a packed key cannot hold. */
int dax_macro_pack_key(uint32_t keycode, uint8_t *packed);
