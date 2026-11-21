#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

LOG_MODULE_REGISTER(dax_split_idle_conn, CONFIG_ZMK_LOG_LEVEL);

/* Timeout is in 10 ms units and intervals in 1.25 ms units: timeout * 10 > 2 * (1 + latency) *
 * interval * 1.25, the least the Bluetooth Core Specification allows. */
BUILD_ASSERT(CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT * 4 >
                 (1 + CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY) * CONFIG_ZMK_DAX_SPLIT_IDLE_CONN_INTERVAL,
             "ZMK_SPLIT_BLE_PREF_TIMEOUT is too short for ZMK_DAX_SPLIT_IDLE_CONN_INTERVAL");

/* Service discovery right after a peripheral connects takes many round trips; widening the
 * interval before it is done would only slow it down. */
#define AFTER_CONNECT_DELAY K_SECONDS(5)

static uint16_t wanted_interval(void) {
    return zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE
               ? CONFIG_ZMK_SPLIT_BLE_PREF_INT
               : CONFIG_ZMK_DAX_SPLIT_IDLE_CONN_INTERVAL;
}

static void update_conn(struct bt_conn *conn, void *data) {
    const uint16_t interval = *(const uint16_t *)data;
    struct bt_conn_info info;

    if (bt_conn_get_info(conn, &info) < 0 || info.role != BT_CONN_ROLE_CENTRAL ||
        info.state != BT_CONN_STATE_CONNECTED || info.le.interval == interval) {
        return;
    }

    const struct bt_le_conn_param param = BT_LE_CONN_PARAM_INIT(
        interval, interval, CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY, CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT);
    const int err = bt_conn_le_param_update(conn, &param);
    if (err < 0) {
        LOG_WRN("Failed to set split interval %u (%d)", interval, err);
        return;
    }
    LOG_DBG("Split interval %u -> %u", info.le.interval, interval);
}

static void apply_cb(struct k_work *work) {
    uint16_t interval = wanted_interval();
    bt_conn_foreach(BT_CONN_TYPE_LE, update_conn, &interval);
}

static K_WORK_DELAYABLE_DEFINE(apply_work, apply_cb);

static int idle_conn_listener_cb(const zmk_event_t *eh) {
    k_work_reschedule(&apply_work, K_NO_WAIT);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dax_split_idle_conn, idle_conn_listener_cb);
ZMK_SUBSCRIPTION(dax_split_idle_conn, zmk_activity_state_changed);

static void connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;

    if (err || bt_conn_get_info(conn, &info) < 0 || info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }
    if (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE) {
        k_work_reschedule(&apply_work, AFTER_CONNECT_DELAY);
    }
}

BT_CONN_CB_DEFINE(dax_split_idle_conn_cb) = {
    .connected = connected,
};
