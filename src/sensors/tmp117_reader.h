#ifndef TMP117_READER_H
#define TMP117_READER_H

#include <zephyr/kernel.h>

#define TMP_COUNT  3

/**
 * @brief Check the three TMP117 precision temperature sensors
 *        (mux ch0/ch2/ch3, ADD0=GND -> 0x48).
 *
 * @return Bitmask of present sensors.
 */
uint8_t tmp117_reader_init(void);

/**
 * @brief Read one sensor.
 *
 * @param idx        Sensor index 0-2.
 * @param temp_c100  Temperature in 0.01 degC.
 * @return 0 on success, negative errno on failure.
 */
int tmp117_reader_read(int idx, int32_t *temp_c100);

#endif /* TMP117_READER_H */
