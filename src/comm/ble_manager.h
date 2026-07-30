#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <zephyr/kernel.h>

typedef void (*ble_rx_cb_t)(const uint8_t *data, uint16_t len);
typedef void (*ble_tx_sent_cb_t)(void);
typedef void (*ble_tx_ready_cb_t)(bool ready);

/**
 * @brief Initialise Bluetooth: NUS service + connectable advertising.
 *
 * Requests 15 ms connection interval and 2M PHY on connect; LED2
 * indicates a live connection; re-advertises on disconnect.
 *
 * @param rx_cb     Called for data written to the NUS RX characteristic.
 * @param sent_cb   Called from the stack when a notification finished
 *                  transmitting (credit return for the TX pacer).
 * @param ready_cb  Called when the TX path becomes usable/unusable
 *                  (subscribe/unsubscribe/disconnect).
 * @return 0 on success, negative errno on failure.
 */
int ble_manager_init(ble_rx_cb_t rx_cb, ble_tx_sent_cb_t sent_cb,
                     ble_tx_ready_cb_t ready_cb);

/** @brief True when connected AND the peer enabled NUS notifications. */
bool ble_manager_can_send(void);

/**
 * @brief Send one NUS notification (whole buffer = one frame).
 *
 * @return 0 on success, negative errno otherwise.
 */
int ble_manager_send(const uint8_t *data, uint16_t len);

#endif /* BLE_MANAGER_H */
