#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include <zephyr/kernel.h>

/**
 * @brief Start the SD logging service (writer thread).
 *
 * Log-by-default: whenever a FAT-formatted card is present, every
 * stream frame handed to sd_logger_write() is appended to rotating
 * files under /SD:/LOG (8.3 names BnnnSmmm.BIN, boot counter derived
 * from the directory). Card absent/removed -> re-probe every 10 s.
 * When free space runs low the oldest file is deleted (circular).
 */
void sd_logger_start(void);

/**
 * @brief Append one stream frame to the log (lock-free SPSC ring).
 *
 * Single-producer: must only be called from the sensor thread. Never
 * blocks; bytes are dropped (and counted) if the writer can't keep up.
 */
void sd_logger_write(const uint8_t *frame, uint16_t len);

/** @brief True while a card is mounted and a log file is open. */
bool sd_logger_active(void);

/**
 * @brief Writer-thread liveness counter (increments every loop pass).
 *
 * If this stops advancing while data is flowing, the writer is stuck
 * inside the SD driver — diagnostic for hot-unplug behavior.
 */
uint32_t sd_logger_writer_beats(void);

#endif /* SD_LOGGER_H */
