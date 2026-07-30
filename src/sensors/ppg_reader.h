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
 *        per mask mux channel).
 *
 * @return Number of ready sensors (0-4).
 */
int ppg_reader_init(void);

/**
 * @brief Fetch one FIFO sample from every ready sensor.
 *
 * @param out  Array of PPG_COUNT samples; absent sensors get valid=false.
 * @return Number of sensors read successfully.
 */
int ppg_reader_read(struct ppg_sample out[PPG_COUNT]);

#endif /* PPG_READER_H */
