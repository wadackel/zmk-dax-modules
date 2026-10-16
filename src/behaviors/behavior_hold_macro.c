#define DT_DRV_COMPAT dax_behavior_hold_macro

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cormoran/zmk/runtime_macro.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

/* Named "Macro" so a settings app picks each parameter by macro name, as it does for &rmacro. */
static const struct behavior_parameter_value_metadata macro_values[] = {
    {
        .display_name = "Macro",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range =
            {
                .min = 0,
                .max = CONFIG_ZMK_RUNTIME_MACRO_COUNT - 1,
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

struct hold_macro_play {
    uint32_t slot;
    struct zmk_behavior_binding_event event;
};

/* The runtime macro player refuses a play with -EBUSY while an earlier macro is still running, and a
 * quick tap lets go well within the press macro's run. Dropping the release would leave its
 * modifier held, and retrying each refused play on its own would let the next press overtake a
 * release that is still waiting, so plays wait here and go out strictly in order. */
K_MSGQ_DEFINE(hold_macro_plays, sizeof(struct hold_macro_play), 8, 4);

static void hold_macro_play_pending(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hold_macro_work, hold_macro_play_pending);

static void hold_macro_play_pending(struct k_work *work) {
    ARG_UNUSED(work);

    struct hold_macro_play play;
    while (k_msgq_peek(&hold_macro_plays, &play) == 0) {
        int ret = zmk_runtime_macro_play(play.slot, &play.event);
        if (ret == -EBUSY) {
            k_work_schedule(&hold_macro_work, K_MSEC(5));
            return;
        }
        if (ret < 0) {
            LOG_WRN("Hold macro: runtime macro %u failed: %d", play.slot, ret);
        }
        k_msgq_get(&hold_macro_plays, &play, K_NO_WAIT);
    }
}

static void hold_macro_enqueue(uint32_t slot, const struct zmk_behavior_binding_event *event) {
    struct hold_macro_play play = {.slot = slot, .event = *event};
    if (k_msgq_put(&hold_macro_plays, &play, K_NO_WAIT) < 0) {
        LOG_WRN("Hold macro: queue full, dropped runtime macro %u", slot);
        return;
    }

    /* Not k_work_reschedule: that would cut short a retry delay already pending. */
    k_work_schedule(&hold_macro_work, K_NO_WAIT);
}

static int on_hold_macro_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    hold_macro_enqueue(binding->param1, &event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_hold_macro_released(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    hold_macro_enqueue(binding->param2, &event);
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
