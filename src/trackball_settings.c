#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <dax/settings.h>
#include <zmk/event_manager.h>

LOG_MODULE_REGISTER(trackball_settings, CONFIG_ZMK_LOG_LEVEL);

#define TRACKBALL_NODE DT_NODELABEL(trackball)
#define SUBSYSTEM_ID "dax_trackball"

// The driver exports this symbol but keeps its header private to its own library.
int paw32xx_set_resolution(const struct device *dev, uint16_t res_cpi);

// The driver's accepted range: 16..127 steps of 38 CPI.
#define CPI_MIN 608
#define CPI_MAX 4826

#if IS_ENABLED(CONFIG_ZMK_STUDIO_RPC)
#include <zmk/studio/custom.h>

static struct zmk_rpc_custom_subsystem_meta trackball_settings_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

// Registered only so a client can tell that the keyboard has these settings; they are read and
// written through the dax_settings subsystem.
static bool trackball_settings_handle_request(const zmk_custom_CallRequest *req,
                                              pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);
    return false;
}

ZMK_RPC_CUSTOM_SUBSYSTEM(dax_trackball, &trackball_settings_meta,
                         trackball_settings_handle_request);
#endif

static int cpi_default(uint16_t slot, uint8_t *buf, size_t size) {
    ARG_UNUSED(slot);
    if (size < sizeof(int32_t)) {
        return -ENOSPC;
    }
    sys_put_le32(DT_PROP(TRACKBALL_NODE, res_cpi), buf);
    return sizeof(int32_t);
}

static const struct dax_setting_type cpi_type = {
    .kind = DAX_SETTING_INT32,
    .min = CPI_MIN,
    .max = CPI_MAX,
    .default_value = cpi_default,
};

DAX_SETTING_DEFINE_TYPED(trackball_cpi_setting, SUBSYSTEM_ID, "cpi", sizeof(int32_t), &cpi_type);

static void apply_cpi(void) {
    const struct device *trackball = DEVICE_DT_GET(TRACKBALL_NODE);

    if (!device_is_ready(trackball)) {
        return;
    }

    int32_t cpi =
        dax_setting_read_int32(&trackball_cpi_setting, 0, DT_PROP(TRACKBALL_NODE, res_cpi));
    int ret = paw32xx_set_resolution(trackball, (uint16_t)cpi);
    if (ret < 0) {
        LOG_WRN("Failed to set CPI to %d: %d", cpi, ret);
    }
}

static int trackball_settings_listener(const zmk_event_t *eh) {
    const struct dax_setting_changed *changed = as_dax_setting_changed(eh);

    if (changed == NULL || strcmp(changed->owner, SUBSYSTEM_ID) == 0) {
        apply_cpi();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(trackball_settings, trackball_settings_listener);
ZMK_SUBSCRIPTION(trackball_settings, dax_setting_changed);
ZMK_SUBSCRIPTION(trackball_settings, dax_settings_loaded);
