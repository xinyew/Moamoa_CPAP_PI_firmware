#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <zephyr/kernel.h>

#include "ppg_reader.h"

/* One 10 ms sampling tick. (ff/vref fields are legacy placeholders so
 * the dormant comm layer still compiles; the kmm-pmask redesign has no
 * FSR channels.)
 */
struct tick_sample {
    struct ppg_sample ppg[PPG_COUNT];
    int16_t ff_mv[3];
    int16_t vref_mv;
    int32_t baro_pa[6];
};

/* Latest values from every sensor, updated by the sampling thread. */
struct system_sensor_data {
    /* PPG (MAX30101 x4, 100 Hz) */
    struct ppg_sample ppg[PPG_COUNT];

    /* Legacy FSR fields (unused on kmm-pmask; kept for comm compat) */
    int32_t ff_mv[3];
    int32_t vref_mv;
    int32_t rfsr_ohm[3];

    /* MS5611 x4 I2C (pressure 100 Hz, temperature 1 Hz) */
    int32_t baro_pa[6];
    int32_t baro_temp_c100[6];
    uint8_t baro_ok_mask;

    /* SHT40 x3 (1 Hz each, staggered) */
    int32_t sht_temp_c100_n[3];
    int32_t sht_rh_x100_n[3];
    uint8_t sht_mask;

    /* TMP117 x3 (1 Hz) */
    int32_t tmp_c100_n[3];
    uint8_t tmp_mask;

    /* Battery + mask presence (1 Hz) */
    int32_t vbat_mv;
    bool mask_present;
    bool sd_ok;   /* microSD initialised at boot (set by main) */

    /* Legacy single-SHT fields (comm compat) */
    int32_t sht_temp_c100;
    int32_t sht_rh_x100;
    bool sht_ok;

    /* Measured rates over the last summary window (per second) */
    uint32_t ppg_rate;
    uint32_t fsr_rate;
    uint32_t baro_rate;
};

extern struct system_sensor_data g_sensor_data;

/**
 * @brief Initialize all sensors and start the 100 Hz sampling thread.
 *
 * Prints a status summary to RTT every second (rates + latest values).
 *
 * @return 0 on success (starts even with sensors absent).
 */
int sensor_manager_start(void);

#endif /* SENSOR_MANAGER_H */
