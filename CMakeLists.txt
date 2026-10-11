# Compiled into ZMK's `app` target rather than a standalone zephyr_library so that
# ZMK's own headers (zmk/ble.h, zmk/keymap.h, ...) are on the include path.
target_sources_ifdef(CONFIG_ZMK_OS_LAYER app PRIVATE src/os_layer.c)
target_sources_ifdef(CONFIG_ZMK_TRACKBALL_SETTINGS app PRIVATE src/trackball_settings.c)
