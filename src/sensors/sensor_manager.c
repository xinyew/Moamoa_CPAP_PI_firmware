/*
 * Sensor manager — one sampling thread driving the CPAP-PI sensor
 * set at the budgeted rates:
 *
 *   tick = 10 ms (100 Hz), absolute-deadline scheduled
 *   - PPG: one FIFO sample per ready MAX30101 every tick (100 Hz x4)
 *   - MS5611 x4 (I2C): read previous conversion + start next every
 *     4th tick (OSR 2048) -> 25 Hz pressure; one cycle per second
 *     converts temperature instead
 *   - SHT40 x3: one sensor per second-third (each 1 Hz, blocking
 *     ~8.3 ms in its tick — the absolute scheduler absorbs it)
 *   - TMP117 x3 + battery + presence: 1 Hz
 *
 * BLE/RTT streaming is disabled pending the protocol redesign for the
 * 4-site topology (see comm_manager.c) — RTT prints are the bring-up
 * interface.
 */

#include "sensor_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "ppg_reader.h"
#include "sht40_reader.h"
#include "tmp117_reader.h"
#include "../drivers/bus_diag.h"
#include "../drivers/driver_batt.h"
#include "../drivers/driver_ms5611.h"
#include "../comm/comm_manager.h"
#include "../storage/sd_logger.h"

LOG_MODULE_REGISTER(sensor_mgr, LOG_LEVEL_INF);

#define TICK_MS            10
#define TICKS_PER_SECOND   (1000 / TICK_MS)

struct system_sensor_data g_sensor_data;

static K_THREAD_STACK_DEFINE(sensor_stack, 4096);
static struct k_thread sensor_thread;


static void print_summary(uint32_t seconds)
{
    struct system_sensor_data *d = &g_sensor_data;

    printk("[%4us] PPG", seconds);
    for (int i = 0; i < PPG_COUNT; i++) {
        if (d->ppg[i].valid) {
            printk(" %d:R%u/I%u/G%u", i + 1, d->ppg[i].red,
                   d->ppg[i].ir, d->ppg[i].green);
        } else {
            printk(" %d:--", i + 1);
        }
    }
    printk(" @%uHz\n", d->ppg_rate);

    printk("        P(Pa)");
    for (int i = 0; i < MS5611_COUNT; i++) {
        if (d->baro_ok_mask & BIT(i)) {
            printk(" %d", d->baro_pa[i]);
        } else {
            printk(" --");
        }
    }
    printk(" @%uHz | SHT", d->baro_rate);
    for (int i = 0; i < SHT_COUNT; i++) {
        if (d->sht_mask & BIT(i)) {
            printk(" %d:%d.%02uC/%u.%01u%%", i + 1,
                   d->sht_temp_c100_n[i] / 100,
                   (unsigned)(d->sht_temp_c100_n[i] % 100),
                   (unsigned)d->sht_rh_x100_n[i] / 100,
                   ((unsigned)d->sht_rh_x100_n[i] % 100) / 10);
        } else {
            printk(" %d:--", i + 1);
        }
    }
    printk("\n        TMP");
    for (int i = 0; i < TMP_COUNT; i++) {
        if (d->tmp_mask & BIT(i)) {
            printk(" %d:%d.%02uC", i + 1, d->tmp_c100_n[i] / 100,
                   (unsigned)(d->tmp_c100_n[i] % 100));
        } else {
            printk(" %d:--", i + 1);
        }
    }
    printk(" | VBAT %d mV | mask %s\n", d->vbat_mv,
           d->mask_present ? "OK" : "ABSENT");
}

static void sensor_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    struct system_sensor_data *d = &g_sensor_data;

    ppg_reader_init();
    d->sht_mask = sht40_reader_init();
    d->tmp_mask = tmp117_reader_init();
    drv_ms5611_init();
    d->baro_ok_mask = drv_ms5611_ok_mask();
    if (drv_batt_init() < 0) {
        LOG_ERR("battery ADC init failed");
    }

    uint32_t tick = 0;
    uint32_t seconds = 0;
    uint32_t ppg_cnt = 0, baro_cnt = 0;
    bool baro_pending = false;
    bool want_baro_temp = false;
    bool standby = false;
    uint8_t absent_secs = 0;
    int64_t next_wake = k_uptime_get();
    int64_t next_second = k_uptime_get() + 1000;

    while (1) {
        struct tick_sample ts = {0};

        /* STANDBY (mask absent >= 5 s): PPGs are shut down (the ~6 mA
         * LED drive stops), baro/SHT/TMP sampling pauses, no DATA
         * frames. Presence + battery keep running at 1 Hz and STATUS
         * frames keep flowing so portal/SD show the state.
         */
        if (standby) {
            goto slow_work;
        }

        /* PPG every tick (100 Hz x4) */
        if (ppg_reader_read(d->ppg) > 0) {
            ppg_cnt++;
        }
        memcpy(ts.ppg, d->ppg, sizeof(ts.ppg));

        /* Baro at 25 Hz (every 4th tick): read the conversion started
         * 40 ms ago (>> 4.6 ms at OSR 2048), start the next. Contact
         * pressure is quasi-static; 25 Hz halves the ~22% conversion
         * duty (~0.6 mA saved across the four sensors) vs 50 Hz.
         */
        if (d->baro_ok_mask != 0 && (tick & 3) == 0) {
            if (baro_pending && drv_ms5611_finish_conv() == 0) {
                for (int i = 0; i < MS5611_COUNT; i++) {
                    drv_ms5611_get(i, &d->baro_pa[i], &d->baro_temp_c100[i]);
                }
                baro_cnt++;
            }

            baro_pending = (drv_ms5611_start_conv(want_baro_temp) == 0);
            want_baro_temp = false;
        }
        memcpy(ts.baro_pa, d->baro_pa, sizeof(ts.baro_pa));

        /* Batch this tick into the BLE/RTT stream */
        comm_manager_push_tick(&ts);

        /* SHT40: one sensor per second-third (each at 1 Hz) */
        uint32_t phase;

slow_work:
        phase = tick % TICKS_PER_SECOND;

        if (!standby && (phase == 20 || phase == 53 || phase == 86)) {
            int idx = (phase == 20) ? 0 : (phase == 53) ? 1 : 2;

            if (d->sht_mask & BIT(idx)) {
                sht40_reader_read(idx, &d->sht_temp_c100_n[idx],
                                  &d->sht_rh_x100_n[idx]);
            }
        }

        /* TMP117 one-shot: trigger at phase 40, results are ready
         * ~124 ms later (8-sample averaging) — read at phase 70.
         */
        if (!standby && phase == 40) {
            tmp117_reader_trigger();
        }

        /* TMP117 + battery + presence at 1 Hz */
        if (phase == 70) {
            if (!standby) {
                for (int i = 0; i < TMP_COUNT; i++) {
                    if (d->tmp_mask & BIT(i)) {
                        tmp117_reader_read(i, &d->tmp_c100_n[i]);
                    }
                }
            }
            drv_batt_read(&d->vbat_mv);
            d->mask_present = (bus_diag_sample_presence() == 1);
            d->sd_ok = sd_logger_active();  /* live logging state */

            /* Standby transitions */
            if (d->mask_present) {
                absent_secs = 0;
                if (standby) {
                    standby = false;
                    ppg_reader_set_shutdown(false);
                    LOG_INF("mask attached - sensing resumed");
                }
            } else if (!standby) {
                if (++absent_secs >= 5) {
                    standby = true;
                    ppg_reader_set_shutdown(true);
                    for (int i = 0; i < PPG_COUNT; i++) {
                        d->ppg[i].valid = false;
                    }
                    LOG_INF("mask absent 5 s - STANDBY (PPGs off)");
                }
            }
        }

        tick++;
        /* Wall-clock second boundary — rates are per REAL second, so
         * they expose loop overruns instead of hiding them (a tick
         * loop stuck at 46 Hz used to still print "100 Hz").
         */
        if (k_uptime_get() >= next_second) {
            next_second += 1000;
            seconds++;
            d->ppg_rate = ppg_cnt;
            d->baro_rate = baro_cnt;
            ppg_cnt = baro_cnt = 0;
            print_summary(seconds);
            comm_manager_push_status(d->sd_ok);
            want_baro_temp = true;  /* one temperature cycle per second */
        }

        /* Absolute-deadline scheduling: 10 ms period regardless of
         * work time; overruns (e.g. the 8.3 ms SHT read) skip ahead.
         */
        next_wake += TICK_MS;
        if (next_wake <= k_uptime_get()) {
            next_wake = k_uptime_get() + TICK_MS;
        }
        k_sleep(K_TIMEOUT_ABS_MS(next_wake));
    }
}

int sensor_manager_start(void)
{
    k_thread_create(&sensor_thread, sensor_stack,
                    K_THREAD_STACK_SIZEOF(sensor_stack),
                    sensor_thread_fn, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
    k_thread_name_set(&sensor_thread, "sensors");
    return 0;
}
