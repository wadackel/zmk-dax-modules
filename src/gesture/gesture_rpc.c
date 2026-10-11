/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * Payloads are raw bytes, little-endian, with the kind in the first byte.
 *
 *   Requests                          Response
 *     01 <on>     recognition events    00
 *     02          begin a capture       00
 *     03          cancel a capture      00
 *     anything else                     01
 *
 *   Notifications
 *     01 <profile> <pattern:4>   a stroke was recognized; pattern is everything drawn so far
 *     02 <profile> <slot>        a gesture fired
 *     03 <pattern:4>             a stroke was captured
 *     04 <pattern:4>             the capture ended; pattern 0 when nothing was drawn
 *
 * Recognition events (01, 02) are sent only after a client turned them on; capture events are
 * always sent, since a capture only starts on request.
 */

#include <string.h>

#include <pb_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include <zmk/event_manager.h>
#include <zmk/studio/custom.h>
#include <zmk/workqueue.h>

#include "gesture_rpc.h"

LOG_MODULE_DECLARE(dax_gesture, CONFIG_ZMK_LOG_LEVEL);

#define REQUEST_SET_EVENTS 0x01
#define REQUEST_CAPTURE_BEGIN 0x02
#define REQUEST_CAPTURE_CANCEL 0x03

#define EVENT_STROKE 0x01
#define EVENT_FIRED 0x02
#define EVENT_CAPTURE_STROKE 0x03
#define EVENT_CAPTURE_DONE 0x04

#define MESSAGE_MAX 6

struct gesture_message {
    uint8_t size;
    uint8_t bytes[MESSAGE_MAX];
};

static atomic_t events_enabled;
static uint8_t response_byte;

static bool encode_bytes(pb_ostream_t *stream, const pb_field_t *field, const uint8_t *bytes,
                         size_t size) {
    return pb_encode_tag_for_field(stream, field) && pb_encode_string(stream, bytes, size);
}

static bool encode_response(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);
    return encode_bytes(stream, field, &response_byte, 1);
}

static bool gesture_rpc_handle_request(const zmk_custom_CallRequest *req, pb_callback_t *res);

static struct zmk_rpc_custom_subsystem_meta gesture_rpc_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(dax_gesture, &gesture_rpc_meta, gesture_rpc_handle_request);

static bool gesture_rpc_handle_request(const zmk_custom_CallRequest *req, pb_callback_t *res) {
    uint8_t kind = req->payload.size >= 1 ? req->payload.bytes[0] : 0;

    response_byte = 0x00;
    switch (kind) {
    case REQUEST_SET_EVENTS:
        atomic_set(&events_enabled, req->payload.size >= 2 && req->payload.bytes[1] != 0);
        break;
    case REQUEST_CAPTURE_BEGIN:
        dax_gesture_capture_begin();
        break;
    case REQUEST_CAPTURE_CANCEL:
        dax_gesture_capture_cancel();
        break;
    default:
        response_byte = 0x01;
        break;
    }

    res->funcs.encode = encode_response;
    res->arg = NULL;
    return true;
}

/* The index a client knows this subsystem by is its position among the registered ones. */
static uint8_t subsystem_index(void) {
    extern struct zmk_rpc_custom_subsystem _zmk_rpc_custom_subsystem_list_start[];
    extern struct zmk_rpc_custom_subsystem _zmk_rpc_custom_subsystem_list_end[];
    uint8_t index = 0;

    for (struct zmk_rpc_custom_subsystem *subsystem = _zmk_rpc_custom_subsystem_list_start;
         subsystem < _zmk_rpc_custom_subsystem_list_end; subsystem++, index++) {
        if (strcmp(subsystem->identifier, "dax_gesture") == 0) {
            return index;
        }
    }
    return 0;
}

static bool encode_message(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct gesture_message *message = *arg;

    return encode_bytes(stream, field, message->bytes, message->size);
}

/* Events are raised from the input thread, whose stack is too small to encode and send a Studio
 * notification on, so they are queued and sent from ZMK's low priority work queue. */
K_MSGQ_DEFINE(gesture_messages, sizeof(struct gesture_message), 16, 1);

static void send_messages(struct k_work *work) {
    ARG_UNUSED(work);
    struct gesture_message message;

    while (k_msgq_get(&gesture_messages, &message, K_NO_WAIT) == 0) {
        pb_callback_t encode = {.funcs.encode = encode_message, .arg = &message};

        raise_zmk_studio_custom_notification((struct zmk_studio_custom_notification){
            .subsystem_index = subsystem_index(), .encode_payload = encode});
    }
}

static K_WORK_DEFINE(send_work, send_messages);

static void queue_message(const struct gesture_message *message) {
    if (k_msgq_put(&gesture_messages, message, K_NO_WAIT) != 0) {
        LOG_DBG("Gesture event dropped: queue full");
        return;
    }
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &send_work);
}

static void queue_pattern(uint8_t kind, dax_gesture_pattern_t strokes) {
    struct gesture_message message = {.size = 5, .bytes = {kind}};

    sys_put_le32(strokes, &message.bytes[1]);
    queue_message(&message);
}

void dax_gesture_rpc_stroke(uint8_t profile, dax_gesture_pattern_t strokes) {
    if (!atomic_get(&events_enabled)) {
        return;
    }

    struct gesture_message message = {.size = 6, .bytes = {EVENT_STROKE, profile}};

    sys_put_le32(strokes, &message.bytes[2]);
    queue_message(&message);
}

void dax_gesture_rpc_fired(uint8_t profile, uint8_t slot) {
    if (!atomic_get(&events_enabled)) {
        return;
    }
    queue_message(&(struct gesture_message){.size = 3, .bytes = {EVENT_FIRED, profile, slot}});
}

void dax_gesture_rpc_capture_stroke(dax_gesture_pattern_t strokes) {
    queue_pattern(EVENT_CAPTURE_STROKE, strokes);
}

void dax_gesture_rpc_capture_done(dax_gesture_pattern_t strokes) {
    queue_pattern(EVENT_CAPTURE_DONE, strokes);
}
