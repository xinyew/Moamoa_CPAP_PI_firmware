#ifndef BUS_DIAG_H
#define BUS_DIAG_H

#include <zephyr/kernel.h>

/**
 * @brief Enable the PCA9517A I2C repeater and read mask presence.
 *
 * Must run before any I2C traffic to the mask.
 *
 * @return 1 if the mask is attached (both PRESEN pins grounded),
 *         0 if not/partial, negative errno on GPIO failure.
 */
int bus_diag_prepare_mask_bus(void);

/**
 * @brief Sample mask presence (pulsed pull-ups, ~100 us).
 *
 * Pins float between calls so the mask's grounds don't burn the
 * internal pull-ups continuously. Call at a low rate (1 Hz).
 *
 * @return 1 attached, 0 not attached, negative errno on failure.
 */
int bus_diag_sample_presence(void);

/**
 * @brief Scan all four TCA9546A channels for the expected sensor
 *        clusters and check every MAX30101 PART_ID.
 *
 * @return Number of expected devices found (0-14).
 */
int bus_diag_scan_mux(void);

/**
 * @brief Probe the microSD card via the disk subsystem.
 *
 * @return 0 if a card responds, negative errno otherwise.
 */
int bus_diag_sd_check(void);

#endif /* BUS_DIAG_H */
