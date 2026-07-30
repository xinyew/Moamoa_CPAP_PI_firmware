#ifndef DRIVER_MS5611_H
#define DRIVER_MS5611_H

#include <zephyr/kernel.h>

#define MS5611_COUNT  4

/**
 * @brief Initialize the four MS5611 barometers (I2C mode, 0x77, one
 *        per mask mux channel).
 *
 * Resets each sensor, reads and CRC-checks its calibration PROM, and
 * seeds the temperature compensation with one blocking D2 conversion.
 * Absent/faulty sensors are marked and skipped by later calls.
 *
 * @return Number of responsive sensors (0-4).
 */
int drv_ms5611_init(void);

/** @brief Bitmask of responsive sensors (bit N = baro N+1). */
uint8_t drv_ms5611_ok_mask(void);

/**
 * @brief Start a conversion on all responsive sensors.
 *
 * OSR 2048 — max conversion time 4.6 ms; call the matching
 * drv_ms5611_finish_conv() no sooner than 5 ms later.
 *
 * @param temperature  true = D2 (temperature), false = D1 (pressure).
 * @return 0 on success, negative errno on first failure.
 */
int drv_ms5611_start_conv(bool temperature);

/**
 * @brief Read out the previously started conversion on all sensors.
 *
 * @return 0 on success, negative errno on first failure.
 */
int drv_ms5611_finish_conv(void);

/**
 * @brief Latest compensated values for sensor idx (0-3).
 *
 * @param press_pa   Pressure in Pa (= 0.01 mbar resolution).
 * @param temp_c100  Temperature in 0.01 degC.
 */
void drv_ms5611_get(int idx, int32_t *press_pa, int32_t *temp_c100);

#endif /* DRIVER_MS5611_H */
