#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/zmk/custom_settings.h>
#include <zmk/event_manager.h>

LOG_MODULE_REGISTER(trackball_settings, CONFIG_ZMK_LOG_LEVEL);

#define TRACKBALL_NODE DT_NODELABEL(trackball)
#define SUBSYSTEM_ID "dax_trackball"
#define CPI_KEY "cpi"

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

// Registered only so the settings below get a subsystem index a client can address;
// every request goes through the custom-settings subsystem instead.
static bool trackball_settings_handle_request(const zmk_custom_CallRequest *req,
                                              pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);
    return false;
}

ZMK_RPC_CUSTOM_SUBSYSTEM(dax_trackball, &trackball_settings_meta,
                         trackball_settings_handle_request);
#endif

ZMK_CUSTOM_SETTING_DEFINE(trackball_cpi_setting, SUBSYSTEM_ID, CPI_KEY,
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                          ZMK_CUSTOM_SETTING_VALUE_INT32(DT_PROP(TRACKBALL_NODE, res_cpi)),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                          ZMK_CUSTOM_SETTING_RANGE_INT32(CPI_MIN, CPI_MAX));

static void apply_cpi(void) {
    const struct device *trackball = DEVICE_DT_GET(TRACKBALL_NODE);
    struct zmk_custom_setting_value value;

    if (!device_is_ready(trackball)) {
        return;
    }

    int ret = zmk_custom_setting_read_by_key(SUBSYSTEM_ID, CPI_KEY, &value);
    if (ret < 0) {
        LOG_WRN("Failed to read the CPI setting: %d", ret);
        return;
    }

    ret = paw32xx_set_resolution(trackball, (uint16_t)value.int32_value);
    if (ret < 0) {
        LOG_WRN("Failed to set CPI to %d: %d", value.int32_value, ret);
    }
}

static int trackball_settings_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *changed = as_zmk_custom_setting_changed(eh);

    if (changed == NULL) {
        // Settings finished loading: the saved value, if any, replaces the devicetree one.
        apply_cpi();
    } else if (changed->kind != ZMK_CUSTOM_SETTING_CHANGED_SAVED &&
               zmk_custom_setting_matches(changed->setting, SUBSYSTEM_ID, CPI_KEY, NULL)) {
        apply_cpi();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(trackball_settings, trackball_settings_listener);
ZMK_SUBSCRIPTION(trackball_settings, zmk_custom_setting_changed);
ZMK_SUBSCRIPTION(trackball_settings, zmk_custom_settings_initialized);
