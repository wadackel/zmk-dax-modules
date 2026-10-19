/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * The dax_settings custom Studio subsystem: the typed settings of every dax feature, by owner and
 * key (proto/dax/settings/settings.proto). It sends no notifications.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>

#include <dax/settings.h>
#include <dax/settings/settings.pb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(sizeof(((dax_settings_Setting *)0)->value.bytes) == DAX_SETTING_TYPED_MAX_SIZE,
             "settings.options must give values DAX_SETTING_TYPED_MAX_SIZE bytes");
BUILD_ASSERT(sizeof(((dax_settings_Setting *)0)->default_value.bytes) ==
                 DAX_SETTING_TYPED_MAX_SIZE,
             "settings.options must give defaults DAX_SETTING_TYPED_MAX_SIZE bytes");
BUILD_ASSERT(sizeof(((dax_settings_WriteRequest *)0)->value.bytes) == DAX_SETTING_TYPED_MAX_SIZE,
             "settings.options must give written values DAX_SETTING_TYPED_MAX_SIZE bytes");

#define PAGE_SIZE ARRAY_SIZE(((dax_settings_ListResponse *)0)->settings)

static bool dax_settings_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                            pb_callback_t *encode_response);

static struct zmk_rpc_custom_subsystem_meta dax_settings_meta = {
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(dax_settings, &dax_settings_meta, dax_settings_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(dax_settings, dax_settings_Response);

static void set_error(dax_settings_Response *resp, dax_settings_ErrorCode code,
                      const char *message) {
    dax_settings_ErrorResponse err = dax_settings_ErrorResponse_init_zero;
    err.code = code;
    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = dax_settings_Response_error_tag;
    resp->response_type.error = err;
}

static void set_status(dax_settings_Response *resp, uint32_t affected_count) {
    resp->which_response_type = dax_settings_Response_status_tag;
    resp->response_type.status =
        (dax_settings_StatusResponse){.affected_count = affected_count};
}

static bool typed_by(const struct dax_setting *setting, const char *owner) {
    return setting->type && strcmp(setting->owner, owner) == 0;
}

static bool owner_exists(const char *owner) {
    STRUCT_SECTION_FOREACH(dax_setting, setting) {
        if (typed_by(setting, owner)) {
            return true;
        }
    }
    return false;
}

static const struct dax_setting *find_setting(const char *owner, const char *key) {
    STRUCT_SECTION_FOREACH(dax_setting, setting) {
        if (typed_by(setting, owner) && strcmp(setting->key, key) == 0) {
            return setting;
        }
    }
    return NULL;
}

static dax_settings_Kind kind_to_proto(enum dax_setting_kind kind) {
    switch (kind) {
    case DAX_SETTING_BOOL:
        return dax_settings_Kind_KIND_BOOL;
    case DAX_SETTING_BYTES:
        return dax_settings_Kind_KIND_BYTES;
    default:
        return dax_settings_Kind_KIND_INT32;
    }
}

static void fill_setting(const struct dax_setting *setting, uint16_t slot,
                         dax_settings_Setting *message) {
    const struct dax_setting_type *type = setting->type;
    *message = (dax_settings_Setting)dax_settings_Setting_init_zero;
    snprintf(message->key, sizeof(message->key), "%s", setting->key);
    message->slot = slot;
    message->kind = kind_to_proto(type->kind);
    message->min = type->min;
    message->max = type->max;
    message->read_only = type->read_only;
    message->written = dax_setting_is_set(setting, slot);
    message->unsaved = dax_setting_is_dirty(setting, slot);

    int len = dax_setting_read(setting, slot, message->value.bytes, sizeof(message->value.bytes),
                               &message->unreadable);
    message->value.size = MAX(len, 0);

    len = type->default_value ? type->default_value(slot, message->default_value.bytes,
                                                    sizeof(message->default_value.bytes))
                              : -ENOENT;
    message->has_default = len >= 0;
    message->default_value.size = MAX(len, 0);
}

static void handle_list(const dax_settings_ListRequest *req, dax_settings_Response *resp) {
    if (!owner_exists(req->owner)) {
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_NOT_FOUND, "No such owner");
        return;
    }

    dax_settings_ListResponse *list = &resp->response_type.list;
    *list = (dax_settings_ListResponse)dax_settings_ListResponse_init_zero;
    resp->which_response_type = dax_settings_Response_list_tag;

    uint32_t index = 0;
    STRUCT_SECTION_FOREACH(dax_setting, setting) {
        if (!typed_by(setting, req->owner)) {
            continue;
        }
        for (uint16_t slot = 0; slot < setting->count; slot++, index++) {
            if (index >= req->offset && list->settings_count < PAGE_SIZE) {
                fill_setting(setting, slot, &list->settings[list->settings_count++]);
            }
        }
    }
    list->total = index;
}

/* The setting a write or a reset names, or NULL after answering why there is none. */
static const struct dax_setting *target(const char *owner, const char *key, uint32_t slot,
                                        dax_settings_Response *resp) {
    const struct dax_setting *setting = find_setting(owner, key);
    if (!setting || slot >= setting->count) {
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_NOT_FOUND, "No such setting");
        return NULL;
    }
    if (setting->type->read_only) {
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_READ_ONLY, "The setting is read only");
        return NULL;
    }
    return setting;
}

static void handle_write(const dax_settings_WriteRequest *req, dax_settings_Response *resp) {
    const struct dax_setting *setting = target(req->owner, req->key, req->slot, resp);
    if (!setting) {
        return;
    }
    if (dax_setting_check(setting, req->slot, req->value.bytes, req->value.size) < 0) {
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_INVALID, "Not a value of this setting");
        return;
    }
    int ret = dax_setting_set(setting, req->slot, req->value.bytes, req->value.size);
    if (ret < 0) {
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_FAILED, "Write failed");
        return;
    }
    set_status(resp, 1);
}

static void handle_reset(const dax_settings_ResetRequest *req, dax_settings_Response *resp) {
    const struct dax_setting *setting = target(req->owner, req->key, req->slot, resp);
    if (!setting) {
        return;
    }
    int ret = dax_setting_unset(setting, req->slot);
    if (ret < 0) {
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_FAILED, "Reset failed");
        return;
    }
    set_status(resp, 1);
}

static void handle_save(const dax_settings_SaveRequest *req, dax_settings_Response *resp) {
    if (!owner_exists(req->owner)) {
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_NOT_FOUND, "No such owner");
        return;
    }
    uint32_t written = 0;
    int ret = dax_settings_save(req->owner, &written);
    if (ret < 0) {
        char message[48];
        snprintf(message, sizeof(message), "Save failed: %d", ret);
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_FAILED, message);
        return;
    }
    set_status(resp, written);
}

static void handle_discard(const dax_settings_DiscardRequest *req, dax_settings_Response *resp) {
    if (!owner_exists(req->owner)) {
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_NOT_FOUND, "No such owner");
        return;
    }
    uint32_t changed = 0;
    dax_settings_discard(req->owner, &changed);
    set_status(resp, changed);
}

static bool dax_settings_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                            pb_callback_t *encode_response) {
    dax_settings_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(dax_settings, encode_response);

    dax_settings_Request req = dax_settings_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, dax_settings_Request_fields, &req)) {
        LOG_WRN("dax_settings: failed to decode a request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_INVALID, "Failed to decode request");
        return true;
    }

    switch (req.which_request_type) {
    case dax_settings_Request_list_tag:
        handle_list(&req.request_type.list, resp);
        break;
    case dax_settings_Request_write_tag:
        handle_write(&req.request_type.write, resp);
        break;
    case dax_settings_Request_reset_tag:
        handle_reset(&req.request_type.reset, resp);
        break;
    case dax_settings_Request_save_tag:
        handle_save(&req.request_type.save, resp);
        break;
    case dax_settings_Request_discard_tag:
        handle_discard(&req.request_type.discard, resp);
        break;
    default:
        set_error(resp, dax_settings_ErrorCode_ERROR_CODE_INVALID, "Unknown request");
        break;
    }
    return true;
}
