/*
 * MAX30101 PPG reader — thin wrapper over the in-tree Zephyr driver.
 * Four instances, one per TCA9546A channel (mask sites); the mux
 * driver switches channels transparently per transaction. Sensors
 * that failed probe at boot are skipped and rejoin after fix+reboot.
 */

#include "ppg_reader.h"

#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ppg_reader, LOG_LEVEL_INF);

static const struct device *const ppg_dev[PPG_COUNT] = {
    DEVICE_DT_GET(DT_NODELABEL(ppg1)),
    DEVICE_DT_GET(DT_NODELABEL(ppg2)),
    DEVICE_DT_GET(DT_NODELABEL(ppg3)),
    DEVICE_DT_GET(DT_NODELABEL(ppg4)),
};

int ppg_reader_init(void)
{
    int ready = 0;

    for (int i = 0; i < PPG_COUNT; i++) {
        if (device_is_ready(ppg_dev[i])) {
            ready++;
        } else {
            LOG_WRN("ppg%d absent", i + 1);
        }
    }

    LOG_INF("PPG: %d/%d online (100 Hz, R/IR/G)", ready, PPG_COUNT);
    return ready;
}

int ppg_reader_read(struct ppg_sample out[PPG_COUNT])
{
    int read = 0;

    for (int i = 0; i < PPG_COUNT; i++) {
        out[i].valid = false;
        if (!device_is_ready(ppg_dev[i])) {
            continue;
        }

        struct sensor_value red, ir, green;

        if (sensor_sample_fetch(ppg_dev[i]) < 0 ||
            sensor_channel_get(ppg_dev[i], SENSOR_CHAN_RED, &red) < 0 ||
            sensor_channel_get(ppg_dev[i], SENSOR_CHAN_IR, &ir) < 0 ||
            sensor_channel_get(ppg_dev[i], SENSOR_CHAN_GREEN, &green) < 0) {
            continue;
        }

        out[i].red = (uint32_t)red.val1;
        out[i].ir = (uint32_t)ir.val1;
        out[i].green = (uint32_t)green.val1;
        out[i].valid = true;
        read++;
    }

    return read;
}
