#ifndef DRIVER_BATT_H
#define DRIVER_BATT_H

#include <zephyr/kernel.h>

/**
 * @brief Initialize the battery-sense ADC channel (AIN7, VBAT/2 via
 *        the 1M/1M divider).
 *
 * @return 0 on success, negative errno on failure.
 */
int drv_batt_init(void);

/**
 * @brief Read the battery voltage.
 *
 * @param vbat_mv  Battery voltage in millivolts (divider-corrected).
 * @return 0 on success, negative errno on failure.
 */
int drv_batt_read(int32_t *vbat_mv);

#endif /* DRIVER_BATT_H */
