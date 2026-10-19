/*
 * Copyright (c) 2026 wadackel
 * SPDX-License-Identifier: MIT
 *
 * The dax_system custom Studio subsystem. Its one request reboots the keyboard, normally or into
 * the UF2 bootloader, so a host can restart it or copy a new firmware on without a key press.
 */

#include <errno.h>
#include <stdio.h>

#include <pb_decode.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zmk/studio/custom.h>

#if IS_ENABLED(CONFIG_RETENTION_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#endif

#include <dax/system/system.pb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* The value the Adafruit nRF52 bootloader reads to stay in UF2 mode, as &bootloader uses it on a
 * board without a retained boot mode (dt-bindings/zmk/reset.h, RST_UF2). */
#define DAX_SYSTEM_REBOOT_UF2 0x57

/* Long enough for the answer to leave over USB before the port goes away. */
#define DAX_SYSTEM_REBOOT_DELAY K_MSEC(200)

static bool dax_system_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                          pb_callback_t *encode_response);

static struct zmk_rpc_custom_subsystem_meta dax_system_meta = {
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(dax_system, &dax_system_meta, dax_system_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(dax_system, dax_system_Response);

static bool to_bootloader;

/* The same two paths as ZMK's &bootloader and &sys_reset (app/src/behaviors/behavior_reset.c). */
static void reboot_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!to_bootloader) {
        sys_reboot(SYS_REBOOT_WARM);
        return;
    }
#if IS_ENABLED(CONFIG_RETENTION_BOOT_MODE)
    int ret = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
    if (ret < 0) {
        LOG_ERR("dax_system: setting the bootloader mode failed (%d)", ret);
        return;
    }
    sys_reboot(SYS_REBOOT_WARM);
#else
    sys_reboot(DAX_SYSTEM_REBOOT_UF2);
#endif
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

static void set_error(dax_system_Response *resp, const char *message) {
    dax_system_ErrorResponse err = dax_system_ErrorResponse_init_zero;
    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = dax_system_Response_error_tag;
    resp->response_type.error = err;
}

static void handle_reboot(const dax_system_RebootRequest *req, dax_system_Response *resp) {
    switch (req->mode) {
    case dax_system_RebootMode_REBOOT_MODE_RESTART:
        to_bootloader = false;
        break;
    case dax_system_RebootMode_REBOOT_MODE_BOOTLOADER:
        to_bootloader = true;
        break;
    default:
        set_error(resp, "Unknown reboot mode");
        return;
    }

    LOG_INF("dax_system: rebooting%s", to_bootloader ? " into the bootloader" : "");
    k_work_schedule(&reboot_work, DAX_SYSTEM_REBOOT_DELAY);
    resp->which_response_type = dax_system_Response_reboot_tag;
    resp->response_type.reboot = (dax_system_RebootResponse)dax_system_RebootResponse_init_zero;
}

static bool dax_system_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                          pb_callback_t *encode_response) {
    dax_system_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(dax_system, encode_response);

    dax_system_Request req = dax_system_Request_init_zero;
    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, dax_system_Request_fields, &req)) {
        LOG_WRN("dax_system: failed to decode a request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, "Failed to decode request");
        return true;
    }

    switch (req.which_request_type) {
    case dax_system_Request_reboot_tag:
        handle_reboot(&req.request_type.reboot, resp);
        break;
    default:
        set_error(resp, "Unknown request");
        break;
    }
    return true;
}
