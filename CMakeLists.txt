# Compiled into ZMK's `app` target rather than a standalone zephyr_library so that
# ZMK's own headers (zmk/ble.h, zmk/keymap.h, ...) are on the include path.
target_sources_ifdef(CONFIG_ZMK_OS_LAYER app PRIVATE src/os_layer.c)
target_sources_ifdef(CONFIG_ZMK_TRACKBALL_SETTINGS app PRIVATE src/trackball_settings.c)

if(CONFIG_ZMK_DAX_GESTURE AND ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
  target_include_directories(app PRIVATE include)
  target_sources(app PRIVATE
    src/gesture/gesture_core.c
    src/gesture/gesture_model.c
    src/gesture/input_processor_gesture.c
  )
  target_sources_ifdef(CONFIG_ZMK_DAX_GESTURE_RUNTIME app PRIVATE src/gesture/gesture_settings.c)
endif()
