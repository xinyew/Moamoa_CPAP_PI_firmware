/*
 * BLE manager — Nordic UART Service (NUS) transport for the sensor
 * stream. Replaces the placeholder custom GATT service: the web
 * portal subscribes to the NUS TX characteristic and receives binary
 * frames (see comm_protocol.h); commands arrive on NUS RX.
 *
 * LED2 = connected indicator (this board has no buttons; comm mode is
 * BLE-only since the module's USB is not wired).
 */

#include "ble_manager.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

#include <bluetooth/services/nus.h>

#include "../drivers/driver_led.h"

LOG_MODULE_REGISTER(ble_mgr, LOG_LEVEL_INF);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static ble_rx_cb_t rx_callback;
static ble_tx_sent_cb_t sent_callback;
static ble_tx_ready_cb_t ready_callback;
static atomic_t connected_flag;
static atomic_t notif_enabled;

/* -------------------------------------------------------------------------- */
/*  NUS callbacks                                                             */
/* -------------------------------------------------------------------------- */

static void on_nus_received(struct bt_conn *conn, const uint8_t *const data,
                            uint16_t len)
{
    ARG_UNUSED(conn);

    if (rx_callback != NULL) {
        rx_callback(data, len);
    }
}

static void on_nus_sent(struct bt_conn *conn)
{
    ARG_UNUSED(conn);

    if (sent_callback != NULL) {
        sent_callback();
    }
}

static void on_nus_send_enabled(enum bt_nus_send_status status)
{
    bool enabled = (status == BT_NUS_SEND_STATUS_ENABLED);

    atomic_set(&notif_enabled, enabled);
    LOG_INF("NUS notifications %s", enabled ? "enabled" : "disabled");
    if (ready_callback != NULL) {
        ready_callback(enabled);
    }
}

static struct bt_nus_cb nus_callbacks = {
    .received = on_nus_received,
    .sent = on_nus_sent,
    .send_enabled = on_nus_send_enabled,
};

/* -------------------------------------------------------------------------- */
/*  Connection callbacks                                                      */
/* -------------------------------------------------------------------------- */

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("BLE connection failed: %u", err);
        return;
    }

    LOG_INF("BLE connected");
    atomic_set(&connected_flag, 1);
    drv_led_set(LED_2, true);

    /* Request low-latency connection params (15 ms interval) */
    struct bt_le_conn_param param = {
        .interval_min = 12,
        .interval_max = 24,
        .latency = 0,
        .timeout = 400,
    };
    bt_conn_le_param_update(conn, &param);

#if defined(CONFIG_BT_USER_PHY_UPDATE)
    /* 2M PHY halves airtime per frame — more margin for the 25/s
     * stream on congested hosts (Windows/Web Bluetooth).
     */
    int phy_ret = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);

    if (phy_ret) {
        LOG_WRN("2M PHY request failed: %d (staying on 1M)", phy_ret);
    }
#endif
}

/* Advertising backoff: fast (30-60 ms) for the first 30 s after boot
 * or a disconnect so tablets connect instantly, then slow (~1 s
 * interval, ~30 uA average vs several hundred) — scanners still find
 * the board, discovery just takes a second or two longer.
 */
#define ADV_FAST_PERIOD_S  30

static struct bt_le_adv_param adv_param;  /* saved for re-advertise */

static int start_advertising(bool fast)
{
    adv_param = (struct bt_le_adv_param)BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_CONN,
        fast ? BT_GAP_ADV_FAST_INT_MIN_1 : BT_GAP_ADV_SLOW_INT_MIN,
        fast ? BT_GAP_ADV_FAST_INT_MAX_1 : BT_GAP_ADV_SLOW_INT_MAX,
        NULL);
    return bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad),
                           sd, ARRAY_SIZE(sd));
}

static void restart_advertise(struct k_work *work)
{
    ARG_UNUSED(work);
    int ret = start_advertising(true);

    if (ret) {
        LOG_ERR("BLE re-advertise failed: %d", ret);
    } else {
        LOG_INF("BLE re-advertising (fast)");
    }
}

static void slow_advertise(struct k_work *work)
{
    ARG_UNUSED(work);

    if (atomic_get(&connected_flag)) {
        return;  /* connected meanwhile — nothing to slow down */
    }
    bt_le_adv_stop();
    if (start_advertising(false) == 0) {
        LOG_INF("BLE advertising slowed (power save)");
    }
}

static K_WORK_DELAYABLE_DEFINE(adv_restart_work, restart_advertise);
static K_WORK_DELAYABLE_DEFINE(adv_slow_work, slow_advertise);

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    LOG_INF("BLE disconnected (reason %u)", reason);

    atomic_set(&connected_flag, 0);
    atomic_set(&notif_enabled, 0);
    drv_led_set(LED_2, false);
    if (ready_callback != NULL) {
        ready_callback(false);
    }

    /* Defer re-advertise — must not run in the stack's own context */
    k_work_schedule(&adv_restart_work, K_MSEC(50));
    k_work_reschedule(&adv_slow_work, K_SECONDS(ADV_FAST_PERIOD_S));
}

static struct bt_conn_cb conn_callbacks = {
    .connected    = connected,
    .disconnected = disconnected,
};

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

int ble_manager_init(ble_rx_cb_t rx_cb, ble_tx_sent_cb_t sent_cb,
                     ble_tx_ready_cb_t ready_cb)
{
    int ret;

    rx_callback = rx_cb;
    sent_callback = sent_cb;
    ready_callback = ready_cb;

    ret = bt_enable(NULL);
    if (ret) {
        LOG_ERR("BLE enable failed: %d", ret);
        return ret;
    }

    bt_conn_cb_register(&conn_callbacks);

    ret = bt_nus_init(&nus_callbacks);
    if (ret) {
        LOG_ERR("NUS init failed: %d", ret);
        return ret;
    }

    ret = start_advertising(true);
    if (ret) {
        LOG_ERR("BLE advertising start failed: %d", ret);
        return ret;
    }
    k_work_schedule(&adv_slow_work, K_SECONDS(ADV_FAST_PERIOD_S));

    LOG_INF("BLE advertising as \"%s\" (NUS)", CONFIG_BT_DEVICE_NAME);
    return 0;
}

bool ble_manager_can_send(void)
{
    return atomic_get(&connected_flag) && atomic_get(&notif_enabled);
}

int ble_manager_send(const uint8_t *data, uint16_t len)
{
    return bt_nus_send(NULL, data, len);
}
