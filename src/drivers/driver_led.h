#ifndef DRIVER_LED_H
#define DRIVER_LED_H

#include <zephyr/kernel.h>

/* Debug LEDs (active low): P0.19, P0.21, P0.20, P0.22 */
#define LED_1    0   /* DEBUG_LED0 — heartbeat */
#define LED_2    1   /* DEBUG_LED1 — BLE connected */
#define LED_3    2   /* DEBUG_LED2 */
#define LED_4    3   /* DEBUG_LED3 */

/**
 * @brief Configure all debug LEDs as outputs (off).
 *
 * @return 0 on success, negative errno on failure.
 */
int drv_led_init(void);

/** @brief Set an LED on or off. */
int drv_led_set(uint8_t idx, bool on);

/** @brief Toggle an LED. */
int drv_led_toggle(uint8_t idx);

#endif /* DRIVER_LED_H */
