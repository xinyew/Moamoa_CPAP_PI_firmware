/*
 * TMP117 precision skin-temperature readers — in-tree ti,tmp11x
 * driver, three sensors (mask sites on mux ch0/ch2/ch3). +-0.1 degC
 * accuracy; polled at 1 Hz.
 */

#include "tmp117_reader.h"

#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tmp117_reader, LOG_LEVEL_INF);

static const struct device *const tmp_dev[TMP_COUNT] = {
    DEVICE_DT_GET(DT_NODELABEL(tmp1)),
    DEVICE_DT_GET(DT_NODELABEL(tmp2)),
    DEVICE_DT_GET(DT_NODELABEL(tmp3)),
};

uint8_t tmp117_reader_init(void)
{
    uint8_t mask = 0;

    for (int i = 0; i < TMP_COUNT; i++) {
        if (device_is_ready(tmp_dev[i])) {
            mask |= BIT(i);
        } else {
            LOG_WRN("tmp%d absent", i + 1);
        }
    }

    LOG_INF("TMP117: mask 0x%X online (1 Hz)", mask);
    return mask;
}

int tmp117_reader_read(int idx, int32_t *temp_c100)
{
    if (idx < 0 || idx >= TMP_COUNT || !device_is_ready(tmp_dev[idx])) {
        return -ENODEV;
    }

    int ret = sensor_sample_fetch(tmp_dev[idx]);

    if (ret < 0) {
        return ret;
    }

    struct sensor_value t;

    ret = sensor_channel_get(tmp_dev[idx], SENSOR_CHAN_AMBIENT_TEMP, &t);
    if (ret < 0) {
        return ret;
    }

    *temp_c100 = t.val1 * 100 + t.val2 / 10000;
    return 0;
}
