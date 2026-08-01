#ifndef COMM_PROTOCOL_H
#define COMM_PROTOCOL_H

/*
 * Binary stream protocol v2 — kmm-pmask 4-site topology (little-endian).
 * Sent over BLE NUS (one frame per notification) and RTT up-buffer 1
 * (byte stream; re-framed on the magic by scripts/rtt_bridge.py).
 *
 * DATA frame (type 0x11) — every 40 ms (25/s), batching 4 ticks:
 *
 *   off size field
 *     0  u16  magic 0xC9A5
 *     2  u8   type = 0x11
 *     3  u8   seq (wraps)
 *     4  u32  t_ms (uptime of last tick in frame)
 *     8  u8   ppg_valid_mask (bit N = ppg N+1 live)
 *     9  u8   n_samples (= 4)
 *    10  u8   baro_ok_mask (bit N = baro N+1 live)
 *    11  u8   reserved
 *    12  PPG block: sensor 0..3, sample 0..3: red, ir, green as u24
 *         = 4 x 4 x 3 x 3 B = 144 B (absent sensors zero-filled)
 *   156  BARO block: sample 0..3, baro 1..4: pressure as u24 Pa
 *         = 4 x 4 x 3 B = 48 B (100 Hz)
 *   204  total (fits one NUS notification at ATT MTU >= 207)
 *
 * STATUS frame (type 0x12) — every 1 s:
 *
 *     0  u16  magic 0xC9A5
 *     2  u8   type = 0x12
 *     3  u8   seq
 *     4  u32  t_ms
 *     8  3 x { i16 sht_temp (0.01 C), u16 sht_rh (0.01 %) } = 12 B
 *    20  3 x i16 tmp117 temp (0.01 C) = 6 B
 *    26  4 x i16 baro temp (0.01 C) = 8 B
 *    34  u16  vbat_mv
 *    36  u8   flags (bit0 = mask present, bit1 = SD ok)
 *    37  u8   rate_ppg (Hz achieved)
 *    38  u8   rate_baro
 *    39  u8   ppg_valid_mask
 *    40  u8   baro_ok_mask
 *    41  u8   sht_mask
 *    42  u8   tmp_mask
 *    43  u8   ble_drops_last_sec (frames shed, saturating)
 *    44  u8   ble_decim (current AIMD pacing: send every Nth frame)
 *    45  total
 *
 * TSYNC record (type 0x13, 16 B) — SD log only, one per second once
 * the wall clock is known (continuous pairs let the offline reader
 * fit RC-oscillator drift):
 *
 *     0  u16  magic 0xC9A5
 *     2  u8   type = 0x13
 *     3  u8   seq
 *     4  u32  uptime_ms
 *     8  u64  epoch_ms (unix wall clock at that uptime)
 *    16  total
 *
 * NUS RX commands:
 *   'B' — binary streaming (default)
 *   'J' — JSON debug mode: 1 Hz JSON line on BLE instead of frames
 *   'T' + u64 epoch_ms LE — wall-clock sync (portal/tablet sends on
 *         connect; retroactively timestamps the whole boot's uptime
 *         timeline in the SD log)
 */

#define COMM_MAGIC          0xC9A5
#define COMM_TYPE_DATA      0x11
#define COMM_TYPE_STATUS    0x12
#define COMM_TYPE_TSYNC     0x13
#define COMM_TSYNC_FRAME_LEN 16

#define COMM_TICKS_PER_FRAME  4
#define COMM_DATA_FRAME_LEN   204
#define COMM_STATUS_FRAME_LEN 45

#define COMM_CMD_BINARY     'B'
#define COMM_CMD_JSON       'J'
#define COMM_CMD_TIMESYNC   'T'

#endif /* COMM_PROTOCOL_H */
