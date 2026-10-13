#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/zmk/custom_settings.h>
#include <cormoran/zmk/runtime_macro.h>

LOG_MODULE_REGISTER(macro_discard_test, CONFIG_ZMK_LOG_LEVEL);

static int log_macro(uint32_t slot, const char *name, size_t encoded_size, bool has_unsaved_changes,
                     void *user_data) {
    LOG_INF("macro_discard_test %s: %s slot %u, %u bytes, unsaved %d", (const char *)user_data,
            name, slot, (unsigned)encoded_size, has_unsaved_changes);
    return 0;
}

// Stands in for a client over Studio RPC, which native_sim tests do not have: replaces the body of
// the default macro with a bare delay, then discards. The keymap plays the macro afterwards.
static void edit_then_discard(struct k_work *work) {
    static const uint8_t only_a_delay[] = {ZMK_RUNTIME_MACRO_FORMAT_VERSION,
                                           ZMK_RUNTIME_MACRO_OP_DELAY, 5};

    zmk_runtime_macro_for_each(log_macro, "at boot");

    int ret = zmk_runtime_macro_write(CONFIG_ZMK_DAX_MACRO_DISCARD_TEST_NAME, only_a_delay,
                                      sizeof(only_a_delay), ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    LOG_INF("macro_discard_test edit: %d", ret);

    ret = zmk_custom_settings_discard_scope(ZMK_RUNTIME_MACRO_SUBSYSTEM_ID, NULL, NULL, NULL);
    LOG_INF("macro_discard_test discard: %d", ret);
    zmk_runtime_macro_for_each(log_macro, "after discard");
}

static K_WORK_DELAYABLE_DEFINE(edit_then_discard_work, edit_then_discard);

// Default macros are created when settings finish loading, which happens in main(), after every
// init level has run.
static int macro_discard_test_init(void) {
    k_work_schedule(&edit_then_discard_work, K_MSEC(100));
    return 0;
}

SYS_INIT(macro_discard_test_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
