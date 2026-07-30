#ifndef SHT40_READER_H
#define SHT40_READER_H

#include <zephyr/kernel.h>

#define SHT_COUNT  3

/**
 * @brief Check the three SHT40 sensors (mux ch0/ch2/ch3).
 *
 * @return Bitmask of present sensors.
 */
uint8_t sht40_reader_init(void);

/**
 * @brief Blocking measurement (~8.3 ms) on one sensor.
 *
 * @param idx        Sensor index 0-2.
 * @param temp_c100  Temperature in 0.01 degC.
 * @param rh_x100    Relative humidity in 0.01 %RH.
 * @return 0 on success, negative errno on failure.
 */
int sht40_reader_read(int idx, int32_t *temp_c100, int32_t *rh_x100);

#endif /* SHT40_READER_H */
