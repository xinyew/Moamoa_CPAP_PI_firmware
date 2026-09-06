/*
 * MAX30101 PPG reader — thin wrapper over the in-tree Zephyr driver.
 * Four instances, one per TCA9546A channel (mask sites); the mux
 * driver switches channels transparently per transaction. Sensors
 * that failed probe at boot are skipped and rejoin after fix+reboot.
 */

#include "ppg_reader.h"

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ppg_reader, LOG_LEVEL_INF);

static const struct device *const ppg_dev[PPG_COUNT] = {
    DEVICE_DT_GET(DT_NODELABEL(ppg1)),
    DEVICE_DT_GET(DT_NODELABEL(ppg2)),
    DEVICE_DT_GET(DT_NODELABEL(ppg3)),
    DEVICE_DT_GET(DT_NODELABEL(ppg4)),
};

/* Raw bus access for the SHDN bit (the in-tree driver has no
 * power-mode API): one mux-channel bus per sensor, MODE_CFG @0x09.
 */
static const struct device *const ppg_bus[PPG_COUNT] = {
    DEVICE_DT_GET(DT_NODELABEL(mux_i2c0)),
    DEVICE_DT_GET(DT_NODELABEL(mux_i2c1)),
    DEVICE_DT_GET(DT_NODELABEL(mux_i2c2)),
    DEVICE_DT_GET(DT_NODELABEL(mux_i2c3)),
};

#define MAX30101_ADDR          0x57
#define MAX30101_REG_MODE_CFG  0x09
#define MAX30101_SHDN_MASK     0x80
#define MAX30101_REG_LED1_PA   0x0C  /* LED1/2/3_PA are consecutive */

/* Ambient/dark baseline per sensor (red, ir, green), captured once at
 * init with LEDs forced off. Subtracted from every reading so the
 * higher LED currents below have ADC headroom to spend on signal
 * instead of ambient DC offset (room/sunlight through the mask).
 */
static uint32_t ppg_ambient[PPG_COUNT][3];

void ppg_reader_set_shutdown(bool sleep)
{
    for (int i = 0; i < PPG_COUNT; i++) {
        if (!device_is_ready(ppg_dev[i])) {
            continue;
        }

        uint8_t mode;

        if (i2c_reg_read_byte(ppg_bus[i], MAX30101_ADDR,
                              MAX30101_REG_MODE_CFG, &mode) < 0) {
            continue;
        }
        if (sleep) {
            mode |= MAX30101_SHDN_MASK;
        } else {
            mode &= ~MAX30101_SHDN_MASK;
        }
        i2c_reg_write_byte(ppg_bus[i], MAX30101_ADDR,
                           MAX30101_REG_MODE_CFG, mode);
    }
    LOG_INF("PPG sensors %s", sleep ? "shut down" : "woken");
}

/* Zero all 3 LED currents, let one stale (LED-on) FIFO entry flush,
 * then read one sample: with the slots still firing but no LED drive,
 * the FIFO reports pure ambient/dark current per channel. Restores
 * the devicetree-configured currents before returning.
 */
static void ppg_reader_calibrate_ambient(void)
{
    for (int i = 0; i < PPG_COUNT; i++) {
        if (!device_is_ready(ppg_dev[i])) {
            continue;
        }

        uint8_t pa[3];
        bool have_pa = true;

        for (int r = 0; r < 3; r++) {
            if (i2c_reg_read_byte(ppg_bus[i], MAX30101_ADDR,
                                   MAX30101_REG_LED1_PA + r, &pa[r]) < 0) {
                have_pa = false;
                break;
            }
        }
        if (!have_pa) {
            LOG_WRN("ppg%d ambient calibration skipped (bus read failed)", i + 1);
            continue;
        }

        for (int r = 0; r < 3; r++) {
            i2c_reg_write_byte(ppg_bus[i], MAX30101_ADDR,
                                MAX30101_REG_LED1_PA + r, 0x00);
        }

        struct sensor_value red, ir, green;

        (void)sensor_sample_fetch(ppg_dev[i]);
        k_sleep(K_MSEC(15));

        if (sensor_sample_fetch(ppg_dev[i]) == 0 &&
            sensor_channel_get(ppg_dev[i], SENSOR_CHAN_RED, &red) == 0 &&
            sensor_channel_get(ppg_dev[i], SENSOR_CHAN_IR, &ir) == 0 &&
            sensor_channel_get(ppg_dev[i], SENSOR_CHAN_GREEN, &green) == 0) {
            ppg_ambient[i][0] = (uint32_t)red.val1;
            ppg_ambient[i][1] = (uint32_t)ir.val1;
            ppg_ambient[i][2] = (uint32_t)green.val1;
        } else {
            LOG_WRN("ppg%d ambient calibration read failed", i + 1);
        }

        for (int r = 0; r < 3; r++) {
            i2c_reg_write_byte(ppg_bus[i], MAX30101_ADDR,
                                MAX30101_REG_LED1_PA + r, pa[r]);
        }
    }
    LOG_INF("PPG ambient baseline: R%u/I%u/G%u (site 1)",
            ppg_ambient[0][0], ppg_ambient[0][1], ppg_ambient[0][2]);
}

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

    ppg_reader_calibrate_ambient();

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

        uint32_t r = (uint32_t)red.val1;
        uint32_t ir_v = (uint32_t)ir.val1;
        uint32_t g = (uint32_t)green.val1;

        out[i].red = (r > ppg_ambient[i][0]) ? r - ppg_ambient[i][0] : 0;
        out[i].ir = (ir_v > ppg_ambient[i][1]) ? ir_v - ppg_ambient[i][1] : 0;
        out[i].green = (g > ppg_ambient[i][2]) ? g - ppg_ambient[i][2] : 0;
        out[i].valid = true;
        read++;
    }

    return read;
}
