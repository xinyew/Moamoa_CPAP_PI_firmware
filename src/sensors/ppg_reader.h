#ifndef PPG_READER_H
#define PPG_READER_H

#include <zephyr/kernel.h>

#define PPG_COUNT  4

struct ppg_sample {
    uint32_t red;
    uint32_t ir;
    uint32_t green;
    bool valid;
};

/**
 * @brief Check which MAX30101 instances came up (in-tree driver, one
 *        per mask mux channel), then capture each one's ambient/dark
 *        baseline (LEDs briefly forced off) for ppg_reader_read().
 *
 * @return Number of ready sensors (0-4).
 */
int ppg_reader_init(void);

/**
 * @brief Fetch one FIFO sample from every ready sensor.
 *
 * Values are ambient-subtracted against the dark baseline captured at
 * init (see ppg_reader_init()), not raw ADC counts.
 *
 * @param out  Array of PPG_COUNT samples; absent sensors get valid=false.
 * @return Number of sensors read successfully.
 */
int ppg_reader_read(struct ppg_sample out[PPG_COUNT]);

/**
 * @brief Put all present sensors in shutdown (LEDs off, ~0.7 uA each)
 *        or wake them.
 *
 * SHDN preserves the configuration registers, so waking resumes with
 * the devicetree-applied settings. This is the dominant power lever:
 * the 4x3 LED drive (~19/19/35 mA per site @5 V, performance-tuned —
 * see the DTS led-pa comment) stops entirely.
 */
void ppg_reader_set_shutdown(bool sleep);

#endif /* PPG_READER_H */
