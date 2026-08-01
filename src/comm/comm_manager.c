/*
 * Comm manager — batches sensor ticks into binary v2 DATA frames
 * (40 ms, see comm_protocol.h) and a 1 Hz STATUS frame, fanned out to:
 *
 *   - RTT up-buffer 1 (wired; decimated to fit the EDU Mini probe's
 *     ~2.3 kB/s drain ceiling)
 *   - BLE NUS via a dedicated credit-paced TX thread:
 *       sensor thread -> drop-OLDEST frame queue (depth 8) -> TX
 *       thread -> <=2 notifications in flight (credits returned by
 *       the NUS sent callback). Nothing in the sensor path or the
 *       system workqueue can ever block on the radio, and a stalled
 *       link degrades by shedding the oldest data with bounded
 *       latency instead of freezing.
 *
 * AIMD link pacing (per second, in push_status): sustained queue
 * drops halve the BLE frame rate (up to 8x); after enough drop-free
 * seconds the rate probes back up, with escalating patience when
 * probes keep failing (no sawtooth on persistently weak links).
 * Current decimation and drops/s ride in the STATUS frame so the
 * portal shows link health live.
 *
 * NUS RX command byte: 'B' binary (default) / 'J' JSON debug.
 */

#include "comm_manager.h"
#include "comm_protocol.h"
#include "ble_manager.h"
#include "rtt_stream.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <stdio.h>
#include <string.h>

#include "../drivers/driver_ms5611.h"
#include "../storage/sd_logger.h"

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

/* Frames built in the sensor thread; RTT written inline (cheap memcpy) */
static uint8_t frame_buf[COMM_DATA_FRAME_LEN];
static uint8_t status_frame_buf[COMM_STATUS_FRAME_LEN];

/* --- BLE TX path: drop-oldest queue + credit-paced sender thread --- */

#define TX_QUEUE_DEPTH   8
#define TX_CREDITS       2    /* notifications in flight */

struct tx_msg {
    uint16_t len;
    uint8_t buf[COMM_DATA_FRAME_LEN];
};

K_MSGQ_DEFINE(tx_msgq, sizeof(struct tx_msg), TX_QUEUE_DEPTH, 4);
static K_SEM_DEFINE(tx_credits, TX_CREDITS, TX_CREDITS);

static uint32_t ble_drops;         /* frames shed (queue full / timeout) */
static uint32_t adapt_last_dropped;
static uint8_t ble_decim = 1;      /* send every Nth built frame, 1..8 */
static uint8_t ble_skip;
static uint8_t adapt_calm;
static uint8_t adapt_calm_need = 5;
static int64_t adapt_last_probe;
static uint8_t last_drops_sec;

/* RTT wired decimation (probe bandwidth, fixed) */
#define RTT_DECIM  3
static uint8_t rtt_skip;

/* Wall clock: epoch_ms - uptime_ms, set by the 'T' command (BT RX
 * context writes, sensor thread reads — 64-bit, so guard with a
 * simple seqlock-free scheme: valid flag written last/read first).
 */
static int64_t epoch_offset_ms;
static atomic_t epoch_valid;

/* Remote sensing enable ('P' command). Boot default ON; survives
 * disconnects on purpose (a reboot always restores sensing).
 */
static atomic_t sensing_enable = ATOMIC_INIT(1);

bool comm_manager_sensing_enabled(void)
{
    return atomic_get(&sensing_enable) != 0;
}

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
/*  BLE TX thread                                                             */
/* -------------------------------------------------------------------------- */

static void enqueue_frame(const uint8_t *buf, uint16_t len)
{
    struct tx_msg m;

    m.len = len;
    memcpy(m.buf, buf, len);

    while (k_msgq_put(&tx_msgq, &m, K_NO_WAIT) != 0) {
        struct tx_msg victim;

        /* Queue full: shed the OLDEST frame — newest data keeps
         * flowing and latency stays bounded.
         */
        if (k_msgq_get(&tx_msgq, &victim, K_NO_WAIT) == 0) {
            ble_drops++;
        } else {
            return;
        }
    }
}

static void tx_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    struct tx_msg m;

    while (1) {
        k_msgq_get(&tx_msgq, &m, K_FOREVER);

        if (!ble_manager_can_send()) {
            continue;  /* link went away — discard */
        }

        /* Credit pacing: never more than TX_CREDITS notifications in
         * flight; a credit that fails to return within 300 ms means
         * the link stalled — shed this frame and move on.
         */
        if (k_sem_take(&tx_credits, K_MSEC(300)) != 0) {
            ble_drops++;
            continue;
        }

        if (ble_manager_send(m.buf, m.len) < 0) {
            ble_drops++;
            k_sem_give(&tx_credits);
        }
    }
}

K_THREAD_DEFINE(comm_tx_thread, 2048, tx_thread_fn, NULL, NULL, NULL,
                K_PRIO_PREEMPT(7), 0, 0);

static void on_tx_sent(void)
{
    k_sem_give(&tx_credits);  /* capped at TX_CREDITS by the sem limit */
}

static void on_tx_ready(bool ready)
{
    if (!ready) {
        k_msgq_purge(&tx_msgq);
        /* Restore full credits — in-flight notifications died with
         * the link and their sent callbacks may never come.
         */
        while (k_sem_take(&tx_credits, K_NO_WAIT) == 0) {
        }
        for (int i = 0; i < TX_CREDITS; i++) {
            k_sem_give(&tx_credits);
        }
    }
}

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
    p[36] = (d->mask_present ? BIT(0) : 0) | (sd_ok ? BIT(1) : 0) |
            (d->sensing_on ? BIT(2) : 0);
    p[37] = (uint8_t)MIN(d->ppg_rate, 255U);
    p[38] = (uint8_t)MIN(d->baro_rate, 255U);
    p[39] = ppg_mask;
    p[40] = d->baro_ok_mask;
    p[41] = d->sht_mask;
    p[42] = d->tmp_mask;
    p[43] = last_drops_sec;
    p[44] = ble_decim;
}

static void ble_send_json_line(void)
{
    struct system_sensor_data *d = &g_sensor_data;
    const struct ppg_sample *ppg = &d->ppg[0];
    uint8_t json[160];

    for (int s = 0; s < PPG_COUNT; s++) {
        if (d->ppg[s].valid) {
            ppg = &d->ppg[s];
            break;
        }
    }

    int len = snprintf((char *)json, sizeof(json),
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

    enqueue_frame(json, MIN((uint16_t)len, (uint16_t)sizeof(json)));
}

/* TSYNC record (SD only): emitted every second once the clock is
 * known — the continuous (uptime, epoch) pairs let the offline reader
 * fit RC-oscillator drift.
 */
static void sd_write_tsync(void)
{
    if (!atomic_get(&epoch_valid)) {
        return;
    }

    uint8_t f[COMM_TSYNC_FRAME_LEN];
    uint32_t up = k_uptime_get_32();

    put_u16(f, COMM_MAGIC);
    f[2] = COMM_TYPE_TSYNC;
    f[3] = seq++;
    put_u32(&f[4], up);
    sys_put_le64((uint64_t)(epoch_offset_ms + up), &f[8]);
    sd_logger_write(f, sizeof(f));
}

/* -------------------------------------------------------------------------- */
/*  AIMD link pacing (called once per real second)                            */
/* -------------------------------------------------------------------------- */

static void adapt_pacing(void)
{
    uint32_t drops = ble_drops - adapt_last_dropped;

    adapt_last_dropped = ble_drops;
    last_drops_sec = (uint8_t)MIN(drops, 255U);

    if (drops > 2) {
        if (ble_decim < 8) {
            /* A probe-up that immediately re-dropped: the link really
             * is that slow — require longer calm before retrying.
             */
            if (k_uptime_get() - adapt_last_probe < 10000 &&
                adapt_calm_need < 30) {
                adapt_calm_need += 5;
            }
            ble_decim <<= 1;
            LOG_WRN("link pacing: decim -> %u (%u drops/s)", ble_decim,
                    drops);
        }
        adapt_calm = 0;
    } else if (drops == 0) {
        if (ble_decim > 1 && ++adapt_calm >= adapt_calm_need) {
            ble_decim >>= 1;
            adapt_last_probe = k_uptime_get();
            adapt_calm = 0;
            LOG_INF("link pacing: probe up, decim -> %u", ble_decim);
        }
    } else {
        adapt_calm = 0;
    }
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

    /* SD log: every frame, full rate (the card is the primary record) */
    sd_logger_write(frame_buf, COMM_DATA_FRAME_LEN);

    /* Wired stream (probe-limited, fixed decimation) */
    if (++rtt_skip >= RTT_DECIM) {
        rtt_skip = 0;
        rtt_stream_write(frame_buf, COMM_DATA_FRAME_LEN);
    }

    /* BLE: AIMD-paced enqueue; TX thread does the sending */
    if (ble_manager_can_send() && mode == COMM_MODE_BINARY) {
        if (++ble_skip >= ble_decim) {
            ble_skip = 0;
            enqueue_frame(frame_buf, COMM_DATA_FRAME_LEN);
        }
    }
}

void comm_manager_push_status(bool sd_ok)
{
    adapt_pacing();
    build_status_frame(sd_ok);
    sd_logger_write(status_frame_buf, COMM_STATUS_FRAME_LEN);
    sd_write_tsync();
    rtt_stream_write(status_frame_buf, COMM_STATUS_FRAME_LEN);

    if (!ble_manager_can_send()) {
        return;
    }

    if (mode == COMM_MODE_JSON) {
        ble_send_json_line();
    } else {
        enqueue_frame(status_frame_buf, COMM_STATUS_FRAME_LEN);
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
    case COMM_CMD_POWER:
        if (len >= 2) {
            atomic_set(&sensing_enable, data[1] ? 1 : 0);
            LOG_INF("remote sensing %s", data[1] ? "ENABLED" : "DISABLED");
        }
        break;
    case COMM_CMD_TIMESYNC:
        if (len >= 9) {
            uint64_t epoch = sys_get_le64(&data[1]);

            /* Store only; the sensor thread emits the TSYNC records
             * (keeps the SD ring single-producer).
             */
            epoch_offset_ms = (int64_t)epoch - k_uptime_get();
            atomic_set(&epoch_valid, 1);
            LOG_INF("wall clock synced (epoch %llu ms)", epoch);
        }
        break;
    default:
        LOG_WRN("unknown command 0x%02X", data[0]);
        break;
    }
}

int comm_manager_init(void)
{
    rtt_stream_init();
    return ble_manager_init(on_rx, on_tx_sent, on_tx_ready);
}
