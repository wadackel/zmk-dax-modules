/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * The `dax_gesture` custom Studio subsystem: live recognition events and pattern capture.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gesture_core.h"

#if IS_ENABLED(CONFIG_ZMK_DAX_GESTURE_RPC)

/* Reported from the processor; safe to call from the input thread. No-ops until a client asks. */
void dax_gesture_rpc_stroke(uint8_t profile, dax_gesture_pattern_t strokes);
void dax_gesture_rpc_fired(uint8_t profile, uint8_t slot);
void dax_gesture_rpc_capture_stroke(dax_gesture_pattern_t strokes);
void dax_gesture_rpc_capture_done(dax_gesture_pattern_t strokes);

/* Implemented by the processor. A capture records whatever is drawn next, with the pointer held
 * still, and ends by itself after a pause or when nothing is drawn for a while. */
void dax_gesture_capture_begin(void);
void dax_gesture_capture_cancel(void);

#else

static inline void dax_gesture_rpc_stroke(uint8_t profile, dax_gesture_pattern_t strokes) {}
static inline void dax_gesture_rpc_fired(uint8_t profile, uint8_t slot) {}
static inline void dax_gesture_rpc_capture_stroke(dax_gesture_pattern_t strokes) {}
static inline void dax_gesture_rpc_capture_done(dax_gesture_pattern_t strokes) {}

#endif
