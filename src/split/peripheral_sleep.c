#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>
#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/pm.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/usb.h>

LOG_MODULE_REGISTER(dax_split_peripheral_sleep, CONFIG_ZMK_LOG_LEVEL);

#define SLEEP_TIMEOUT K_MSEC(CONFIG_ZMK_DAX_SPLIT_PERIPHERAL_SLEEP_TIMEOUT)

static bool usb_powered(void) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

static void sleep_cb(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(sleep_work, sleep_cb);

static void sleep_cb(struct k_work *work) {
    if (zmk_split_bt_peripheral_is_connected()) {
        return;
    }
    if (usb_powered()) {
        k_work_reschedule(&sleep_work, SLEEP_TIMEOUT);
        return;
    }

    LOG_INF("Central gone for %d ms, entering deep sleep",
            CONFIG_ZMK_DAX_SPLIT_PERIPHERAL_SLEEP_TIMEOUT);

    /* Listeners such as LED widgets turn their outputs off on this state, as they do before
     * ZMK's own sleep; a GPIO left driven keeps its level through System OFF. */
    raise_zmk_activity_state_changed(
        (struct zmk_activity_state_changed){.state = ZMK_ACTIVITY_SLEEP});

    if (zmk_pm_suspend_devices() < 0) {
        LOG_ERR("Failed to suspend all the devices");
        zmk_pm_resume_devices();
        k_work_reschedule(&sleep_work, SLEEP_TIMEOUT);
        return;
    }

    sys_poweroff();
}

static int peripheral_sleep_listener_cb(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *ev =
        as_zmk_split_peripheral_status_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->connected) {
        k_work_cancel_delayable(&sleep_work);
    } else {
        k_work_reschedule(&sleep_work, SLEEP_TIMEOUT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_split_peripheral_sleep, peripheral_sleep_listener_cb);
ZMK_SUBSCRIPTION(dax_split_peripheral_sleep, zmk_split_peripheral_status_changed);

static int peripheral_sleep_init(void) {
    k_work_schedule(&sleep_work, SLEEP_TIMEOUT);
    return 0;
}

SYS_INIT(peripheral_sleep_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
