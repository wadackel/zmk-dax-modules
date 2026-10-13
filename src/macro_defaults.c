#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/runtime_macro.h>
#include <zmk/event_manager.h>

LOG_MODULE_REGISTER(macro_defaults, CONFIG_ZMK_LOG_LEVEL);

// Static, not on the stack: the listener runs on whichever thread discards, and writing the macro
// back raises events that come through here again.
static uint8_t default_body[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];
static uint8_t current_body[CONFIG_ZMK_RUNTIME_MACRO_MAX_BYTES];

// A discard puts a setting back to its record in flash. A macro from the devicetree has no record
// until it is saved once, and the registry then leaves it as edited. This puts the devicetree body
// back instead.
static int macro_defaults_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *changed = as_zmk_custom_setting_changed(eh);
    const struct zmk_custom_setting *setting = changed->setting;

    if (changed->kind != ZMK_CUSTOM_SETTING_CHANGED_DISCARDED ||
        !zmk_custom_setting_keyspace_of(setting) ||
        !zmk_custom_setting_matches(setting, ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, NULL,
                                    ZMK_RUNTIME_MACRO_KEY_PREFIX) ||
        (setting->state->flags & ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const size_t prefix_size = sizeof(ZMK_RUNTIME_MACRO_KEY_PREFIX) - 1;
    const char *key = zmk_custom_setting_public_key(setting);

    if (strncmp(key, ZMK_RUNTIME_MACRO_KEY_PREFIX, prefix_size) != 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // The registry hands the key out in a buffer it reuses for the next slot it is asked about.
    char name[CONFIG_ZMK_RUNTIME_MACRO_NAME_MAX_LEN + 1];
    strncpy(name, key + prefix_size, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    size_t default_size;
    size_t current_size;

    if (zmk_runtime_macro_default_encode(name, default_body, sizeof(default_body), &default_size) <
            0 ||
        zmk_runtime_macro_read(name, current_body, sizeof(current_body), &current_size) < 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (current_size == default_size && memcmp(current_body, default_body, default_size) == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int ret = zmk_runtime_macro_write(name, default_body, default_size,
                                      ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (ret < 0) {
        LOG_WRN("Failed to put macro \"%s\" back to its default: %d", name, ret);
        return ZMK_EV_EVENT_BUBBLE;
    }

    // The write marked the macro unsaved again. A second discard clears that; it comes back
    // through this listener and stops at the comparison above.
    zmk_custom_setting_discard(setting);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(macro_defaults, macro_defaults_listener);
ZMK_SUBSCRIPTION(macro_defaults, zmk_custom_setting_changed);
