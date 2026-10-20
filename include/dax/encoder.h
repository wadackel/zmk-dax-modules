/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * The dax encoder behavior (dax,behavior-encoder) and its settings, the `dax_encoder` owner.
 */

#pragma once

#include <zmk/keymap.h>
#include <zmk/sensors.h>

#define DAX_ENCODER_SUBSYSTEM_ID "dax_encoder"
/* The most bytes in a sensor's node name, which the read-only `sensor_name` setting holds. */
#define DAX_ENCODER_NAME_SIZE 24
/* A `binding` record, little-endian: behavior local id (2), param1 (4), param2 (4), tap_ms (2). */
#define DAX_ENCODER_RECORD_SIZE 12
#define DAX_ENCODER_MAX_TAP_MS 1000

enum dax_encoder_direction {
    DAX_ENCODER_CW = 0,
    DAX_ENCODER_CCW = 1,
};

/* A sensor, a layer by id (not by position) and a direction, as a slot of `binding`. */
#define DAX_ENCODER_SLOT(sensor, layer_id, direction)                                              \
    (((sensor) * ZMK_KEYMAP_LAYERS_LEN + (layer_id)) * 2 + (direction))
#define DAX_ENCODER_SLOT_COUNT (ZMK_KEYMAP_SENSORS_LEN * ZMK_KEYMAP_LAYERS_LEN * 2)
