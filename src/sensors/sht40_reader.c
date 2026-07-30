/*
 * SHT40 temp/humidity readers — in-tree sht4x driver, three sensors
 * (mask sites on mux ch0/ch2/ch3). Staggered at 1 Hz each by the
 * sensor manager; higher duty would self-heat.
 */

#include "sht40_reader.h"

#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sht40_reader, LOG_LEVEL_INF);

static const struct device *const sht_dev[SHT_COUNT] = {
    DEVICE_DT_GET(DT_NODELABEL(sht1)),
    DEVICE_DT_GET(DT_NODELABEL(sht2)),
    DEVICE_DT_GET(DT_NODELABEL(sht3)),
};

uint8_t sht40_reader_init(void)
{
    uint8_t mask = 0;

    for (int i = 0; i < SHT_COUNT; i++) {
        if (device_is_ready(sht_dev[i])) {
            mask |= BIT(i);
        } else {
            LOG_WRN("sht%d absent", i + 1);
        }
    }

    LOG_INF("SHT40: mask 0x%X online (1 Hz each, staggered)", mask);
    return mask;
}

int sht40_reader_read(int idx, int32_t *temp_c100, int32_t *rh_x100)
{
    if (idx < 0 || idx >= SHT_COUNT || !device_is_ready(sht_dev[idx])) {
        return -ENODEV;
    }

    int ret = sensor_sample_fetch(sht_dev[idx]);

    if (ret < 0) {
        return ret;
    }

    struct sensor_value t, rh;

    ret = sensor_channel_get(sht_dev[idx], SENSOR_CHAN_AMBIENT_TEMP, &t);
    if (ret < 0) {
        return ret;
    }
    ret = sensor_channel_get(sht_dev[idx], SENSOR_CHAN_HUMIDITY, &rh);
    if (ret < 0) {
        return ret;
    }

    *temp_c100 = t.val1 * 100 + t.val2 / 10000;
    *rh_x100 = rh.val1 * 100 + rh.val2 / 10000;
    return 0;
}
