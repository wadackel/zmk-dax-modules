/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 *
 * Adapted from cormoran/zmk-feature-runtime-macro (src/studio/runtime_macro_handler.c at 3853096)
 * under the dax_macro names: the dax_macro custom Studio subsystem
 * (proto/dax/macro/macro.proto). It sends no notifications.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>

#include <dax/macro.h>
#include <dax/macro/macro.pb.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* The list response holds a fixed number of macros, and names and key runs of a fixed size
 * (macro.options). */
BUILD_ASSERT(CONFIG_ZMK_DAX_MACRO_COUNT <= ARRAY_SIZE(((dax_macro_ListMacrosResponse *)0)->macros),
             "a macro list response cannot hold every slot");
BUILD_ASSERT(CONFIG_ZMK_DAX_MACRO_NAME_MAX < sizeof(((dax_macro_MacroSummary *)0)->name),
             "macro.options must give names CONFIG_ZMK_DAX_MACRO_NAME_MAX bytes and a NUL");
BUILD_ASSERT(sizeof(((dax_macro_KeyTapSequenceStep *)0)->packed_keys.bytes) ==
                 DAX_MACRO_KEY_TAP_SEQUENCE_MAX,
             "macro.options must give key runs DAX_MACRO_KEY_TAP_SEQUENCE_MAX bytes");

static bool dax_macro_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                         pb_callback_t *encode_response);

static struct zmk_rpc_custom_subsystem_meta dax_macro_meta = {
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(dax_macro, &dax_macro_meta, dax_macro_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(dax_macro, dax_macro_Response);

static void set_error(dax_macro_Response *resp, dax_macro_ErrorCode code, const char *message) {
    dax_macro_ErrorResponse err = dax_macro_ErrorResponse_init_zero;
    err.code = code;
    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = dax_macro_Response_error_tag;
    resp->response_type.error = err;
}

static void set_errno_error(dax_macro_Response *resp, int err) {
    switch (err) {
    case -ENOENT:
        set_error(resp, dax_macro_ErrorCode_ERROR_CODE_NOT_FOUND, "No such slot or step");
        break;
    case -EINVAL:
        set_error(resp, dax_macro_ErrorCode_ERROR_CODE_INVALID, "Not a step or name it can hold");
        break;
    case -EPERM:
        set_error(resp, dax_macro_ErrorCode_ERROR_CODE_INVALID,
                  "A macro that comes with the firmware cannot be deleted");
        break;
    case -EEXIST:
        set_error(resp, dax_macro_ErrorCode_ERROR_CODE_EXISTS, "Another macro has that name");
        break;
    case -ENOSPC:
        set_error(resp, dax_macro_ErrorCode_ERROR_CODE_NO_ROOM, "No slot is free");
        break;
    case -EMSGSIZE:
        set_error(resp, dax_macro_ErrorCode_ERROR_CODE_TOO_LONG, "Over the size limit");
        break;
    default: {
        char message[32];
        snprintf(message, sizeof(message), "Failed: %d", err);
        set_error(resp, dax_macro_ErrorCode_ERROR_CODE_FAILED, message);
        break;
    }
    }
}

static void set_status(dax_macro_Response *resp, uint32_t affected_count) {
    resp->which_response_type = dax_macro_Response_status_tag;
    resp->response_type.status = (dax_macro_StatusResponse){.affected_count = affected_count};
}

/* For the edits that answer with one affected slot. */
static int status_of(int ret, dax_macro_Response *resp) {
    if (ret == 0) {
        set_status(resp, 1);
    }
    return ret;
}

static dax_macro_MacroSource source_to_proto(enum dax_macro_source source) {
    switch (source) {
    case DAX_MACRO_SOURCE_DEFAULT:
        return dax_macro_MacroSource_MACRO_SOURCE_DEFAULT;
    case DAX_MACRO_SOURCE_OVERRIDDEN:
        return dax_macro_MacroSource_MACRO_SOURCE_OVERRIDDEN;
    case DAX_MACRO_SOURCE_RUNTIME:
        return dax_macro_MacroSource_MACRO_SOURCE_RUNTIME;
    default:
        return dax_macro_MacroSource_MACRO_SOURCE_EMPTY;
    }
}

static void binding_to_proto(const struct dax_macro_step *step,
                             dax_macro_BehaviorBinding *binding) {
    *binding = (dax_macro_BehaviorBinding){
        .behavior_id = step->behavior_id, .param1 = step->param1, .param2 = step->param2};
}

static void step_to_proto(const struct dax_macro_step *step, dax_macro_MacroStep *message) {
    *message = (dax_macro_MacroStep)dax_macro_MacroStep_init_zero;
    switch (step->kind) {
    case DAX_MACRO_STEP_DOWN:
        message->which_step = dax_macro_MacroStep_down_tag;
        binding_to_proto(step, &message->step.down);
        break;
    case DAX_MACRO_STEP_UP:
        message->which_step = dax_macro_MacroStep_up_tag;
        binding_to_proto(step, &message->step.up);
        break;
    case DAX_MACRO_STEP_TAP:
        message->which_step = dax_macro_MacroStep_tap_tag;
        binding_to_proto(step, &message->step.tap);
        break;
    case DAX_MACRO_STEP_DELAY:
        message->which_step = dax_macro_MacroStep_delay_tag;
        message->step.delay.delay_ms = step->delay_ms;
        break;
    default:
        message->which_step = dax_macro_MacroStep_key_tap_sequence_tag;
        message->step.key_tap_sequence.packed_keys.size = step->key_count;
        memcpy(message->step.key_tap_sequence.packed_keys.bytes, step->keys, step->key_count);
        break;
    }
}

static int binding_from_proto(enum dax_macro_step_kind kind,
                              const dax_macro_BehaviorBinding *binding,
                              struct dax_macro_step *step) {
    if (binding->behavior_id > UINT16_MAX) {
        return -EINVAL;
    }
    *step = (struct dax_macro_step){
        .kind = kind,
        .behavior_id = binding->behavior_id,
        .param1 = binding->param1,
        .param2 = binding->param2,
    };
    return 0;
}

static int step_from_proto(bool has_step, const dax_macro_MacroStep *message,
                           struct dax_macro_step *step) {
    if (!has_step) {
        return -EINVAL;
    }
    switch (message->which_step) {
    case dax_macro_MacroStep_down_tag:
        return binding_from_proto(DAX_MACRO_STEP_DOWN, &message->step.down, step);
    case dax_macro_MacroStep_up_tag:
        return binding_from_proto(DAX_MACRO_STEP_UP, &message->step.up, step);
    case dax_macro_MacroStep_tap_tag:
        return binding_from_proto(DAX_MACRO_STEP_TAP, &message->step.tap, step);
    case dax_macro_MacroStep_delay_tag:
        *step = (struct dax_macro_step){.kind = DAX_MACRO_STEP_DELAY,
                                        .delay_ms = message->step.delay.delay_ms};
        return 0;
    case dax_macro_MacroStep_key_tap_sequence_tag: {
        const dax_macro_KeyTapSequenceStep_packed_keys_t *keys =
            &message->step.key_tap_sequence.packed_keys;
        *step = (struct dax_macro_step){.kind = DAX_MACRO_STEP_KEY_TAP_SEQUENCE,
                                        .key_count = keys->size};
        memcpy(step->keys, keys->bytes, keys->size);
        return 0;
    }
    default:
        return -EINVAL;
    }
}

static int handle_list(dax_macro_Response *resp) {
    dax_macro_ListMacrosResponse *list = &resp->response_type.list_macros;
    *list = (dax_macro_ListMacrosResponse)dax_macro_ListMacrosResponse_init_zero;
    list->max_macro_bytes = CONFIG_ZMK_DAX_MACRO_MAX_BYTES;
    list->max_name_length = CONFIG_ZMK_DAX_MACRO_NAME_MAX;
    list->slot_count = CONFIG_ZMK_DAX_MACRO_COUNT;
    for (uint32_t slot = 0; slot < CONFIG_ZMK_DAX_MACRO_COUNT; slot++) {
        dax_macro_MacroSummary *summary = &list->macros[list->macros_count];
        int len = dax_macro_read(slot, summary->name, sizeof(summary->name), NULL, 0);
        if (len == -ENOENT) {
            continue;
        }
        if (len < 0) {
            return len;
        }
        summary->slot = slot;
        summary->encoded_size = len;
        summary->unsaved = dax_macro_is_unsaved(slot);
        summary->source = source_to_proto(dax_macro_source(slot));
        list->macros_count++;
    }
    resp->which_response_type = dax_macro_Response_list_macros_tag;
    return 0;
}

/* The steps of a get are encoded from this copy after the handler returns, so the response holds
 * no array of steps. One request is handled at a time, as for the response buffer. */
static struct {
    uint8_t body[CONFIG_ZMK_DAX_MACRO_MAX_BYTES];
    size_t len;
} get_stream;

static bool encode_steps(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);
    struct dax_macro_step step;
    dax_macro_MacroStep message;
    size_t offset = 0;
    int ret;
    while ((ret = dax_macro_next_step(get_stream.body, get_stream.len, &offset, &step)) == 0) {
        step_to_proto(&step, &message);
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, dax_macro_MacroStep_fields, &message)) {
            return false;
        }
    }
    return ret == -ENOENT;
}

static int handle_get(const dax_macro_GetMacroRequest *req, dax_macro_Response *resp) {
    dax_macro_GetMacroResponse *result = &resp->response_type.get_macro;
    *result = (dax_macro_GetMacroResponse)dax_macro_GetMacroResponse_init_zero;
    int len = dax_macro_read(req->slot, result->macro.name, sizeof(result->macro.name),
                             get_stream.body, sizeof(get_stream.body));
    if (len < 0) {
        return len;
    }
    get_stream.len = len;
    result->has_macro = true;
    result->macro.slot = req->slot;
    result->macro.encoded_size = len;
    result->macro.source = source_to_proto(dax_macro_source(req->slot));
    result->macro.steps.funcs.encode = encode_steps;
    resp->which_response_type = dax_macro_Response_get_macro_tag;
    return 0;
}

static int handle_create(const dax_macro_CreateMacroRequest *req, dax_macro_Response *resp) {
    uint32_t slot;
    int ret = dax_macro_create(req->name, &slot);
    if (ret == 0) {
        set_status(resp, slot);
    }
    return ret;
}

static int handle_set_step(const dax_macro_SetMacroStepRequest *req, dax_macro_Response *resp) {
    struct dax_macro_step step;
    int ret = step_from_proto(req->has_step, &req->step, &step);
    return status_of(ret ?: dax_macro_set_step(req->slot, req->step_index, &step), resp);
}

static int handle_append_step(const dax_macro_AppendMacroStepRequest *req,
                              dax_macro_Response *resp) {
    struct dax_macro_step step;
    int ret = step_from_proto(req->has_step, &req->step, &step);
    return status_of(ret ?: dax_macro_append_step(req->slot, &step), resp);
}

static void handle_get_global_settings(dax_macro_Response *resp) {
    dax_macro_GetMacroGlobalSettingsResponse *result =
        &resp->response_type.get_macro_global_settings;
    *result = (dax_macro_GetMacroGlobalSettingsResponse){
        .has_settings = true,
        .settings =
            {
                .tap_ms = dax_macro_tap_ms(),
                .max_entries = CONFIG_ZMK_DAX_MACRO_COUNT,
                .key_press_behavior_id =
                    zmk_behavior_get_local_id(DEVICE_DT_NAME(DT_NODELABEL(kp))),
            },
    };
    resp->which_response_type = dax_macro_Response_get_macro_global_settings_tag;
}

static int handle_set_tap_ms(const dax_macro_SetTapMsRequest *req, dax_macro_Response *resp) {
    return status_of(req->tap_ms > UINT16_MAX ? -EINVAL : dax_macro_write_tap_ms(req->tap_ms),
                     resp);
}

static bool dax_macro_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                         pb_callback_t *encode_response) {
    dax_macro_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(dax_macro, encode_response);

    dax_macro_Request req = dax_macro_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, dax_macro_Request_fields, &req)) {
        LOG_WRN("dax_macro: failed to decode a request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, dax_macro_ErrorCode_ERROR_CODE_INVALID, "Failed to decode request");
        return true;
    }

    uint32_t count = 0;
    int ret = 0;
    switch (req.which_request_type) {
    case dax_macro_Request_list_macros_tag:
        ret = handle_list(resp);
        break;
    case dax_macro_Request_get_macro_tag:
        ret = handle_get(&req.request_type.get_macro, resp);
        break;
    case dax_macro_Request_create_macro_tag:
        ret = handle_create(&req.request_type.create_macro, resp);
        break;
    case dax_macro_Request_rename_macro_tag:
        ret = status_of(dax_macro_rename(req.request_type.rename_macro.slot,
                                         req.request_type.rename_macro.name),
                        resp);
        break;
    case dax_macro_Request_delete_macro_tag:
        ret = status_of(dax_macro_delete(req.request_type.delete_macro.slot), resp);
        break;
    case dax_macro_Request_reset_macro_tag:
        ret = status_of(dax_macro_reset(req.request_type.reset_macro.slot), resp);
        break;
    case dax_macro_Request_set_macro_step_tag:
        ret = handle_set_step(&req.request_type.set_macro_step, resp);
        break;
    case dax_macro_Request_append_macro_step_tag:
        ret = handle_append_step(&req.request_type.append_macro_step, resp);
        break;
    case dax_macro_Request_set_macro_step_count_tag:
        ret = status_of(dax_macro_set_step_count(req.request_type.set_macro_step_count.slot,
                                                 req.request_type.set_macro_step_count.step_count),
                        resp);
        break;
    case dax_macro_Request_get_macro_global_settings_tag:
        handle_get_global_settings(resp);
        break;
    case dax_macro_Request_set_tap_ms_tag:
        ret = handle_set_tap_ms(&req.request_type.set_tap_ms, resp);
        break;
    case dax_macro_Request_save_macros_tag:
        ret = dax_macro_save(&count);
        if (ret == 0) {
            set_status(resp, count);
        }
        break;
    case dax_macro_Request_discard_macros_tag:
        ret = dax_macro_discard(&count);
        if (ret == 0) {
            set_status(resp, count);
        }
        break;
    default:
        set_error(resp, dax_macro_ErrorCode_ERROR_CODE_INVALID, "Unknown request");
        break;
    }

    if (ret < 0) {
        set_errno_error(resp, ret);
    }
    return true;
}
