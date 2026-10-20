/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * The dax pointer processor (dax,input-processor-pointer) and its settings, the `dax_pointer`
 * owner. A processor is a slot, numbered in devicetree instance order.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>

#define DAX_POINTER_SUBSYSTEM_ID "dax_pointer"
#define DAX_POINTER_COUNT DT_NUM_INST_STATUS_OKAY(dax_input_processor_pointer)
/* The most bytes in a display-name, which the read-only `name` setting holds. */
#define DAX_POINTER_NAME_SIZE 24

/* What a processor does to a report. Each field is one key of the owner. */
struct dax_pointer_values {
    int32_t scale_mul;
    int32_t scale_div;
    int32_t rotation;
    bool xy_swap;
    bool x_invert;
    bool y_invert;
    bool xy_to_scroll;
    /* Bit n is the nth layer in keymap order; 0 means every layer. */
    uint32_t layers;
    bool temp_layer_on;
    /* A layer position in keymap order, not a layer id. */
    uint8_t temp_layer;
    uint16_t temp_layer_after_ms;
    uint16_t temp_layer_back_ms;
};

/* The devicetree's values for the slot, which are its defaults. */
void dax_pointer_defaults(uint16_t slot, struct dax_pointer_values *values);
const char *dax_pointer_name(uint16_t slot);
/* Makes the slot's processor use these values from its next report on. Not for an ISR. */
void dax_pointer_apply(uint16_t slot, const struct dax_pointer_values *values);
