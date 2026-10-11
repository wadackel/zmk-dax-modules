#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(os_layer, CONFIG_LOG_DEFAULT_LEVEL);

#define PROFILE_COUNT 5

static const int8_t profile_layers[PROFILE_COUNT] = {
    CONFIG_ZMK_OS_LAYER_PROFILE_0,
    CONFIG_ZMK_OS_LAYER_PROFILE_1,
    CONFIG_ZMK_OS_LAYER_PROFILE_2,
    CONFIG_ZMK_OS_LAYER_PROFILE_3,
    CONFIG_ZMK_OS_LAYER_PROFILE_4,
};

static void deactivate_all_os_layers(void) {
    for (int i = 0; i < PROFILE_COUNT; i++) {
        if (profile_layers[i] >= 0 && zmk_keymap_layer_active(profile_layers[i])) {
            zmk_keymap_layer_deactivate(profile_layers[i], true);
        }
    }
}

static void update_os_layer(uint8_t profile_index) {
    deactivate_all_os_layers();

    if (profile_index < PROFILE_COUNT && profile_layers[profile_index] >= 0) {
        zmk_keymap_layer_activate(profile_layers[profile_index], true);
    }
}

static int os_layer_listener_cb(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *ev =
        as_zmk_ble_active_profile_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    update_os_layer(ev->index);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(os_layer, os_layer_listener_cb);
ZMK_SUBSCRIPTION(os_layer, zmk_ble_active_profile_changed);

static int os_layer_init(void) {
    update_os_layer(zmk_ble_active_profile_index());
    return 0;
}

SYS_INIT(os_layer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
