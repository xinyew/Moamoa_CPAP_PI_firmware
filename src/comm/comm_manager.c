/*
 * Comm manager — batches sensor ticks into binary v2 DATA frames
 * (40 ms, see comm_protocol.h) and a 1 Hz STATUS frame, fanned out to:
 *
 *   - RTT up-buffer 1 (wired, always on, lossless with a probe reading)
 *   - BLE NUS (when connected; sent from the system workqueue so the
 *     sensor thread never blocks on radio; dropped if in flight)
 *
 * NUS RX command byte: 'B' binary (default) / 'J' JSON debug — JSON
 * mode replaces the BLE stream with a 1 Hz line; RTT stays binary.
 */

#include "comm_manager.h"
#include "comm_protocol.h"
#include "ble_manager.h"
#include "rtt_stream.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include "../drivers/driver_ms5611.h"

LOG_MODULE_REGISTER(comm_mgr, LOG_LEVEL_INF);

BUILD_ASSERT(PPG_COUNT == 4, "v2 frame format assumes 4 PPG sites");
BUILD_ASSERT(MS5611_COUNT == 4, "v2 frame format assumes 4 barometers");

enum comm_mode {
    COMM_MODE_BINARY,
    COMM_MODE_JSON,
};

static enum comm_mode mode = COMM_MODE_BINARY;
static uint8_t seq;

/* Tick accumulator (sensor-thread context only) */
static struct tick_sample acc[COMM_TICKS_PER_FRAME];
static int acc_n;

/* Frames are built in the sensor thread (also fed to RTT), then
 * snapshotted into per-transport buffers for the BLE workqueue.
 */
static uint8_t frame_buf[COMM_DATA_FRAME_LEN];
static uint8_t status_frame_buf[COMM_STATUS_FRAME_LEN];
static uint8_t ble_data_buf[COMM_DATA_FRAME_LEN];
static uint8_t ble_status_buf[160];
static uint16_t ble_status_len;
static atomic_t data_busy;
static atomic_t status_busy;
static uint32_t dropped_frames;

/* -------------------------------------------------------------------------- */
/*  Little-endian helpers                                                     */
/* -------------------------------------------------------------------------- */

static inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static inline void put_u24(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
}

static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

/* -------------------------------------------------------------------------- */
/*  BLE send work                                                             */
/* -------------------------------------------------------------------------- */

static void send_data_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    if (ble_manager_send(ble_data_buf, COMM_DATA_FRAME_LEN) < 0) {
        dropped_frames++;
    }
    atomic_set(&data_busy, 0);
}

static void send_status_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    if (ble_manager_send(ble_status_buf, ble_status_len) < 0) {
        dropped_frames++;
    }
    atomic_set(&status_busy, 0);
}

static K_WORK_DEFINE(send_data_work, send_data_work_fn);
static K_WORK_DEFINE(send_status_work, send_status_work_fn);

/* -------------------------------------------------------------------------- */
/*  Frame builders (v2)                                                       */
/* -------------------------------------------------------------------------- */

static void build_data_frame(void)
{
    uint8_t *p = frame_buf;
    struct system_sensor_data *d = &g_sensor_data;
    uint8_t ppg_mask = 0;

    for (int s = 0; s < PPG_COUNT; s++) {
        if (acc[COMM_TICKS_PER_FRAME - 1].ppg[s].valid) {
            ppg_mask |= BIT(s);
        }
    }

    put_u16(p, COMM_MAGIC);
    p[2] = COMM_TYPE_DATA;
    p[3] = seq++;
    put_u32(&p[4], k_uptime_get_32());
    p[8] = ppg_mask;
    p[9] = COMM_TICKS_PER_FRAME;
    p[10] = d->baro_ok_mask;
    p[11] = 0;

    uint8_t *q = &p[12];

    for (int s = 0; s < PPG_COUNT; s++) {
        for (int k = 0; k < COMM_TICKS_PER_FRAME; k++) {
            const struct ppg_sample *ps = &acc[k].ppg[s];

            put_u24(q, ps->valid ? ps->red : 0);
            put_u24(q + 3, ps->valid ? ps->ir : 0);
            put_u24(q + 6, ps->valid ? ps->green : 0);
            q += 9;
        }
    }

    for (int k = 0; k < COMM_TICKS_PER_FRAME; k++) {
        for (int i = 0; i < MS5611_COUNT; i++) {
            int32_t pa = acc[k].baro_pa[i];

            put_u24(q, pa < 0 ? 0 : (uint32_t)pa);
            q += 3;
        }
    }
}

static void build_status_frame(bool sd_ok)
{
    uint8_t *p = status_frame_buf;
    struct system_sensor_data *d = &g_sensor_data;
    uint8_t ppg_mask = 0;

    for (int s = 0; s < PPG_COUNT; s++) {
        if (d->ppg[s].valid) {
            ppg_mask |= BIT(s);
        }
    }

    put_u16(p, COMM_MAGIC);
    p[2] = COMM_TYPE_STATUS;
    p[3] = seq++;
    put_u32(&p[4], k_uptime_get_32());
    for (int i = 0; i < 3; i++) {
        put_u16(&p[8 + 4 * i], (uint16_t)(int16_t)d->sht_temp_c100_n[i]);
        put_u16(&p[10 + 4 * i], (uint16_t)d->sht_rh_x100_n[i]);
    }
    for (int i = 0; i < 3; i++) {
        put_u16(&p[20 + 2 * i], (uint16_t)(int16_t)d->tmp_c100_n[i]);
    }
    for (int i = 0; i < 4; i++) {
        put_u16(&p[26 + 2 * i], (uint16_t)(int16_t)d->baro_temp_c100[i]);
    }
    put_u16(&p[34], (uint16_t)MAX(d->vbat_mv, 0));
    p[36] = (d->mask_present ? BIT(0) : 0) | (sd_ok ? BIT(1) : 0);
    p[37] = (uint8_t)MIN(d->ppg_rate, 255U);
    p[38] = (uint8_t)MIN(d->baro_rate, 255U);
    p[39] = ppg_mask;
    p[40] = d->baro_ok_mask;
    p[41] = d->sht_mask;
    p[42] = d->tmp_mask;
}

static void ble_send_json_line(void)
{
    if (atomic_cas(&status_busy, 0, 1) == false) {
        return;
    }

    struct system_sensor_data *d = &g_sensor_data;
    const struct ppg_sample *ppg = &d->ppg[0];

    for (int s = 0; s < PPG_COUNT; s++) {
        if (d->ppg[s].valid) {
            ppg = &d->ppg[s];
            break;
        }
    }

    int len = snprintf((char *)ble_status_buf, sizeof(ble_status_buf),
                       "{\"r\":%u,\"i\":%u,\"g\":%u,\"p\":%d,"
                       "\"t\":%d.%02d,\"h\":%d.%02d,\"skin\":%d.%02d,"
                       "\"vbat\":%d}\n",
                       ppg->red, ppg->ir, ppg->green, (int)d->baro_pa[0],
                       (int)(d->sht_temp_c100_n[0] / 100),
                       (int)(d->sht_temp_c100_n[0] % 100),
                       (int)(d->sht_rh_x100_n[0] / 100),
                       (int)(d->sht_rh_x100_n[0] % 100),
                       (int)(d->tmp_c100_n[0] / 100),
                       (int)(d->tmp_c100_n[0] % 100),
                       (int)d->vbat_mv);

    ble_status_len = MIN((uint16_t)len, (uint16_t)sizeof(ble_status_buf));
    k_work_submit(&send_status_work);
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

void comm_manager_push_tick(const struct tick_sample *tick)
{
    acc[acc_n++] = *tick;
    if (acc_n < COMM_TICKS_PER_FRAME) {
        return;
    }
    acc_n = 0;

    build_data_frame();

    /* Wired stream: decimated — the J-Link EDU Mini drains RTT at only
     * ~2.3 kB/s, so send every 3rd DATA frame (~8 fps, 1.7 kB/s).
     * BLE below carries the full 25 fps.
     */
    static uint8_t rtt_decim;

    if (++rtt_decim >= 3) {
        rtt_decim = 0;
        rtt_stream_write(frame_buf, COMM_DATA_FRAME_LEN);
    }

    /* BLE: only when connected, subscribed and in binary mode */
    if (ble_manager_can_send() && mode == COMM_MODE_BINARY) {
        if (atomic_cas(&data_busy, 0, 1)) {
            memcpy(ble_data_buf, frame_buf, COMM_DATA_FRAME_LEN);
            k_work_submit(&send_data_work);
        } else {
            dropped_frames++;
        }
    }
}

void comm_manager_push_status(bool sd_ok)
{
    build_status_frame(sd_ok);
    rtt_stream_write(status_frame_buf, COMM_STATUS_FRAME_LEN);

    if (!ble_manager_can_send()) {
        return;
    }

    if (mode == COMM_MODE_JSON) {
        ble_send_json_line();
    } else if (atomic_cas(&status_busy, 0, 1)) {
        memcpy(ble_status_buf, status_frame_buf, COMM_STATUS_FRAME_LEN);
        ble_status_len = COMM_STATUS_FRAME_LEN;
        k_work_submit(&send_status_work);
    }
}

static void on_rx(const uint8_t *data, uint16_t len)
{
    if (len < 1) {
        return;
    }

    switch (data[0]) {
    case COMM_CMD_BINARY:
        mode = COMM_MODE_BINARY;
        LOG_INF("mode: binary");
        break;
    case COMM_CMD_JSON:
        mode = COMM_MODE_JSON;
        LOG_INF("mode: JSON debug");
        break;
    default:
        LOG_WRN("unknown command 0x%02X", data[0]);
        break;
    }
}

int comm_manager_init(void)
{
    rtt_stream_init();
    return ble_manager_init(on_rx);
}
