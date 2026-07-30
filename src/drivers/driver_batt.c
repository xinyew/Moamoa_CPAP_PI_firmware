/*
 * Battery voltage sense — AIN7 (P0.31) behind a 1M/1M divider.
 * High source impedance (500 kohm): max acquisition time is set in the
 * devicetree channel; readings are for coarse gauge/brown-out margin.
 */

#include "driver_batt.h"

#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(batt, LOG_LEVEL_INF);

static const struct adc_dt_spec batt_chan =
    ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

static int16_t sample_buf;
static struct adc_sequence sequence = {
    .buffer = &sample_buf,
    .buffer_size = sizeof(sample_buf),
};

int drv_batt_init(void)
{
    if (!adc_is_ready_dt(&batt_chan)) {
        LOG_ERR("ADC not ready");
        return -ENODEV;
    }

    int ret = adc_channel_setup_dt(&batt_chan);

    if (ret < 0) {
        LOG_ERR("channel setup failed: %d", ret);
        return ret;
    }

    LOG_INF("battery sense ready (AIN7, /2 divider)");
    return 0;
}

int drv_batt_read(int32_t *vbat_mv)
{
    int ret = adc_sequence_init_dt(&batt_chan, &sequence);

    if (ret < 0) {
        return ret;
    }
    ret = adc_read_dt(&batt_chan, &sequence);
    if (ret < 0) {
        return ret;
    }

    int32_t val = sample_buf < 0 ? 0 : sample_buf;

    ret = adc_raw_to_millivolts_dt(&batt_chan, &val);
    if (ret < 0) {
        return ret;
    }

    *vbat_mv = val * 2;  /* 1M/1M divider */
    return 0;
}
