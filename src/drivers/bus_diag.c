/*
 * Bring-up diagnostics for the CPAP-PI system.
 *
 * - Asserts the PCA9517A repeater enable and reports mask presence
 *   (the mask grounds both PRESEN pins of the symmetric FFC).
 * - Scans each TCA9546A downstream channel for the expected cluster:
 *     ch0/ch2/ch3: MAX30101 (0x57) + MS5611 (0x77) + SHT40 (0x44)
 *                  + TMP117 (0x48)
 *     ch1:         MAX30101 + MS5611
 * - Reads MAX30101 PART_ID (0xFF, expect 0x15) per channel.
 * - Probes the microSD card (SPI) via the disk subsystem.
 */

#include "bus_diag.h"

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bus_diag, LOG_LEVEL_INF);

static const struct gpio_dt_spec pca_en =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pca_en_gpios);
static const struct gpio_dt_spec presen_a =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), presen_a_gpios);
static const struct gpio_dt_spec presen_b =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), presen_b_gpios);

static const struct device *const mux_ch[] = {
    DEVICE_DT_GET(DT_NODELABEL(mux_i2c0)),
    DEVICE_DT_GET(DT_NODELABEL(mux_i2c1)),
    DEVICE_DT_GET(DT_NODELABEL(mux_i2c2)),
    DEVICE_DT_GET(DT_NODELABEL(mux_i2c3)),
};

/* Expected 7-bit addresses per channel (0 = end of list) */
static const uint8_t expected[4][4] = {
    { 0x57, 0x77, 0x44, 0x48 },
    { 0x57, 0x77, 0x00, 0x00 },
    { 0x57, 0x77, 0x44, 0x48 },
    { 0x57, 0x77, 0x44, 0x48 },
};

int bus_diag_prepare_mask_bus(void)
{
    int ret;

    if (!gpio_is_ready_dt(&pca_en) || !gpio_is_ready_dt(&presen_a) ||
        !gpio_is_ready_dt(&presen_b)) {
        LOG_ERR("mask-bus GPIOs not ready");
        return -ENODEV;
    }

    /* Repeater ON before any mask I2C traffic */
    ret = gpio_pin_configure_dt(&pca_en, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        return ret;
    }
    gpio_pin_configure_dt(&presen_a, GPIO_INPUT);
    gpio_pin_configure_dt(&presen_b, GPIO_INPUT);
    k_msleep(2);

    int a = gpio_pin_get_dt(&presen_a);
    int b = gpio_pin_get_dt(&presen_b);

    LOG_INF("PCA9517A enabled; mask presence A=%d B=%d %s", a, b,
            (a == 1 && b == 1) ? "(mask attached)" :
            (a || b) ? "(PARTIAL - check FFC seating!)" : "(no mask)");
    return (a == 1 && b == 1) ? 1 : 0;
}

static bool probe_addr(const struct device *bus, uint8_t addr)
{
    if (i2c_write(bus, NULL, 0, addr) == 0) {
        return true;
    }
    uint8_t byte;
    return i2c_read(bus, &byte, 1, addr) == 0;
}

int bus_diag_scan_mux(void)
{
    int found_expected = 0;

    for (size_t ch = 0; ch < ARRAY_SIZE(mux_ch); ch++) {
        if (!device_is_ready(mux_ch[ch])) {
            LOG_ERR("mux ch%u bus not ready", (unsigned)ch);
            continue;
        }

        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            if (!probe_addr(mux_ch[ch], addr)) {
                continue;
            }
            bool exp = false;

            for (int i = 0; i < 4; i++) {
                if (expected[ch][i] == addr) {
                    exp = true;
                }
            }
            LOG_INF("mux ch%u: found 0x%02X%s", (unsigned)ch, addr,
                    exp ? " (expected)" : addr == 0x70 ? " (mux)" : " (?)");
            if (exp) {
                found_expected++;
            }
        }

        /* Report expected-but-missing explicitly */
        for (int i = 0; i < 4; i++) {
            uint8_t addr = expected[ch][i];

            if (addr != 0 && !probe_addr(mux_ch[ch], addr)) {
                LOG_ERR("mux ch%u: MISSING 0x%02X", (unsigned)ch, addr);
            }
        }
    }

    /* PPG PART_ID check (0xFF -> 0x15) on every channel */
    for (uint8_t ch = 0; ch < 4; ch++) {
        uint8_t id = 0;
        int ret = i2c_reg_read_byte(mux_ch[ch], 0x57, 0xFF, &id);

        if (ret == 0) {
            LOG_INF("ppg%u (ch%u): PART_ID=0x%02X %s", ch + 1, ch, id,
                    id == 0x15 ? "OK" : "UNEXPECTED");
        } else {
            LOG_ERR("ppg%u (ch%u): no response (err %d)", ch + 1, ch, ret);
        }
    }

    LOG_INF("mux scan: %d/14 expected sensors present", found_expected);
    return found_expected;
}

int bus_diag_sd_check(void)
{
    int ret = disk_access_init("SD");

    if (ret != 0) {
        LOG_WRN("SD card: init failed (%d) — card absent or SPI issue", ret);
        return ret;
    }

    uint32_t sector_count = 0;
    uint32_t sector_size = 0;

    disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_COUNT, &sector_count);
    disk_access_ioctl("SD", DISK_IOCTL_GET_SECTOR_SIZE, &sector_size);
    LOG_INF("SD card: %u sectors x %u B (%u MB)", sector_count, sector_size,
            (uint32_t)(((uint64_t)sector_count * sector_size) >> 20));
    return 0;
}
