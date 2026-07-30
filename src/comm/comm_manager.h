#ifndef COMM_MANAGER_H
#define COMM_MANAGER_H

#include <zephyr/kernel.h>

#include "../sensors/sensor_manager.h"

/**
 * @brief Initialise the comm layer (BLE NUS transport + RTT stream).
 *
 * @return 0 on success, negative errno on failure.
 */
int comm_manager_init(void);

/**
 * @brief Feed one 10 ms sensor tick (called from the sensor thread).
 *
 * Accumulates COMM_TICKS_PER_FRAME ticks into a v2 DATA frame, writes
 * it to RTT (always) and BLE (when connected).
 */
void comm_manager_push_tick(const struct tick_sample *tick);

/**
 * @brief Send the 1 Hz STATUS frame (or the JSON debug line when the
 *        peer selected JSON mode) from the latest g_sensor_data.
 *
 * @param sd_ok  Whether the microSD card initialised at boot.
 */
void comm_manager_push_status(bool sd_ok);

#endif /* COMM_MANAGER_H */
