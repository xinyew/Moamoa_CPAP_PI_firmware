/*
 * TMP117 precision skin-temperature readers — in-tree ti,tmp11x
 * driver, three sensors (mask sites on mux ch0/ch2/ch3). +-0.1 degC
 * accuracy.
 *
 * Power: the chips default to continuous conversion (~220 uA each
 * while converting at the default cycle). We poll at 1 Hz, so they
 * run in ONE-SHOT mode instead: trigger, ~124 ms conversion (8-sample
 * averaging), auto-shutdown (~250 nA). The temperature register
 * retains the last result for the read.
 */

#include "tmp117_reader.h"

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/tmp11x.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tmp117_reader, LOG_LEVEL_INF);

static const struct device *const tmp_dev[TMP_COUNT] = {
    DEVICE_DT_GET(DT_NODELABEL(tmp1)),
    DEVICE_DT_GET(DT_NODELABEL(tmp2)),
    DEVICE_DT_GET(DT_NODELABEL(tmp3)),
};

static uint8_t present_mask;

uint8_t tmp117_reader_init(void)
{
    const struct sensor_value none = {0, 0};

    present_mask = 0;
    for (int i = 0; i < TMP_COUNT; i++) {
        if (!device_is_ready(tmp_dev[i])) {
            LOG_WRN("tmp%d absent", i + 1);
            continue;
        }
        present_mask |= BIT(i);

        /* Stop the default continuous conversions */
        if (sensor_attr_set(tmp_dev[i], SENSOR_CHAN_AMBIENT_TEMP,
                            SENSOR_ATTR_TMP11X_SHUTDOWN_MODE, &none) < 0) {
            LOG_WRN("tmp%d: shutdown-mode set failed", i + 1);
        }
    }

    LOG_INF("TMP117: mask 0x%X online (one-shot @1 Hz)", present_mask);
    return present_mask;
}

void tmp117_reader_trigger(void)
{
    const struct sensor_value none = {0, 0};

    for (int i = 0; i < TMP_COUNT; i++) {
        if (present_mask & BIT(i)) {
            sensor_attr_set(tmp_dev[i], SENSOR_CHAN_AMBIENT_TEMP,
                            SENSOR_ATTR_TMP11X_ONE_SHOT_MODE, &none);
        }
    }
}

int tmp117_reader_read(int idx, int32_t *temp_c100)
{
    if (idx < 0 || idx >= TMP_COUNT || !(present_mask & BIT(idx))) {
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
