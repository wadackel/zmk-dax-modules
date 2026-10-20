# Compiled into ZMK's `app` target rather than a standalone zephyr_library so that
# ZMK's own headers (zmk/ble.h, zmk/keymap.h, ...) are on the include path.
target_sources_ifdef(CONFIG_ZMK_OS_LAYER app PRIVATE src/os_layer.c)
target_sources_ifdef(CONFIG_ZMK_TRACKBALL_SETTINGS app PRIVATE src/trackball_settings.c)
target_sources_ifdef(CONFIG_ZMK_DAX_MACRO_DEFAULTS app PRIVATE src/macro_defaults.c)
target_sources_ifdef(CONFIG_ZMK_DAX_MACRO_DISCARD_TEST app PRIVATE src/test/macro_discard_test.c)
target_sources_ifdef(CONFIG_ZMK_DAX_HOLD_MACRO app PRIVATE src/behaviors/behavior_hold_macro.c)

target_include_directories(app PRIVATE include)

if(CONFIG_ZMK_DAX_SETTINGS)
  target_sources(app PRIVATE src/settings/settings.c)
  zephyr_linker_sources(SECTIONS include/linker/dax-settings.ld)
endif()

# The wire definitions of every enabled RPC, generated together at the end of this file: a second
# zephyr_library() in this directory would collide with the first.
set(dax_protos)
if(CONFIG_ZMK_DAX_SYSTEM_RPC)
  target_sources(app PRIVATE src/system/system_rpc.c)
  list(APPEND dax_protos ${CMAKE_CURRENT_SOURCE_DIR}/proto/dax/system/system.proto)
endif()

if(CONFIG_ZMK_DAX_SETTINGS_STUDIO_RPC)
  target_sources(app PRIVATE src/settings/settings_rpc.c)
  list(APPEND dax_protos ${CMAKE_CURRENT_SOURCE_DIR}/proto/dax/settings/settings.proto)
endif()

if(CONFIG_ZMK_DAX_GESTURE AND ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
  target_sources(app PRIVATE
    src/gesture/gesture_core.c
    src/gesture/gesture_model.c
    src/gesture/input_processor_gesture.c
  )
  target_sources_ifdef(CONFIG_ZMK_DAX_GESTURE_RUNTIME app PRIVATE src/gesture/gesture_settings.c)
  target_sources_ifdef(CONFIG_ZMK_DAX_GESTURE_RPC app PRIVATE src/gesture/gesture_rpc.c)
  target_sources_ifdef(CONFIG_ZMK_DAX_GESTURE_TEST app PRIVATE src/test/gesture_test.c)
endif()

if(CONFIG_ZMK_DAX_COMBO)
  target_sources(app PRIVATE src/combo/combo_engine.c src/combo/combo_settings.c)
  target_sources_ifdef(CONFIG_ZMK_DAX_COMBO_TEST app PRIVATE src/test/combo_test.c)

  if(CONFIG_ZMK_DAX_COMBO_STUDIO_RPC)
    target_sources(app PRIVATE src/combo/combo_rpc.c)
    list(APPEND dax_protos ${CMAKE_CURRENT_SOURCE_DIR}/proto/dax/combo/combo.proto)
  endif()

  # ZMK dispatches an event to its listeners in link order, and module sources are added to app
  # before ZMK's own. Left there, the combo engine would see keys before hold-taps do, which is
  # what breaks a quick modifier + combo. Move it to where ZMK's combo.c sits: after hold-taps,
  # before the keymap. ZMK adds its sources after the modules, so this runs deferred, once the
  # application's CMakeLists.txt is done.
  function(dax_place_combo_engine)
    get_target_property(sources app SOURCES)
    list(FILTER sources EXCLUDE REGEX "/src/combo/combo_engine\\.c$")
    get_target_property(engine app SOURCES)
    list(FILTER engine INCLUDE REGEX "/src/combo/combo_engine\\.c$")
    list(FIND sources "src/combo.c" anchor)
    if(anchor EQUAL -1)
      message(FATAL_ERROR "zmk-dax-modules: ZMK's src/combo.c is not in app's sources, so the dax "
                          "combo engine cannot be placed after hold-taps. See CMakeLists.txt.")
    endif()
    list(INSERT sources ${anchor} ${engine})
    set_property(TARGET app PROPERTY SOURCES "${sources}")
  endfunction()
  cmake_language(DEFER DIRECTORY ${APPLICATION_SOURCE_DIR} CALL dax_place_combo_engine)
endif()

if(dax_protos)
  list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
  include(nanopb)
  set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
  set(NANOPB_GENERATE_CPP_STANDALONE OFF)

  # The generated sources go in a library of their own: added to app directly, they break the
  # build.
  zephyr_library()
  nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR} ${dax_protos})
  target_include_directories(${ZEPHYR_CURRENT_LIBRARY} PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
  target_sources(${ZEPHYR_CURRENT_LIBRARY} PRIVATE ${proto_srcs} ${proto_hdrs})
  target_include_directories(app PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/proto)
  add_dependencies(app ${ZEPHYR_CURRENT_LIBRARY})
endif()
