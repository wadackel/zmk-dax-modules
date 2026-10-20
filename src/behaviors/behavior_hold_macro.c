#define DT_DRV_COMPAT dax_behavior_hold_macro

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <dax/macro.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

/* Named "Macro" so a settings app picks each parameter by macro name, as it does for &dax_macro. */
static const struct behavior_parameter_value_metadata macro_values[] = {
    {
        .display_name = "Macro",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range =
            {
                .min = 0,
                .max = CONFIG_ZMK_DAX_MACRO_COUNT - 1,
            },
    },
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = macro_values,
    .param1_values_len = ARRAY_SIZE(macro_values),
    .param2_values = macro_values,
    .param2_values_len = ARRAY_SIZE(macro_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif

static void play(uint32_t slot, const struct zmk_behavior_binding_event *event) {
    int ret = dax_macro_play(slot, event);
    if (ret < 0 && ret != -ENOSPC && ret != -ELOOP) {
        LOG_WRN("Hold macro: dax macro %u: %d", slot, ret);
    }
}

static int on_hold_macro_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    play(binding->param1, &event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_hold_macro_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    play(binding->param2, &event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_hold_macro_driver_api = {
    .binding_pressed = on_hold_macro_pressed,
    .binding_released = on_hold_macro_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

#define HOLD_MACRO_INST(n)                                                                         \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_hold_macro_driver_api);

DT_INST_FOREACH_STATUS_OKAY(HOLD_MACRO_INST)
