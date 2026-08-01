# BLE Protocol (v2.1)

Single source of truth for byte layouts: `src/comm/comm_protocol.h`.
Anything here that disagrees with that header is a doc bug.
Consumers that must stay in sync: portal `useComm.js`,
`scripts/rtt_bridge.py` (FRAME_LEN table), `tools/sd_reader.py`.

## Transport

| Item | Value |
|---|---|
| Advertised name | `CPAP_PI_Control` (filter on prefix `CPAP`) |
| Service | Nordic UART Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| TX (board→host, notify) | `6e400003-…` |
| RX (host→board, write / write-no-rsp) | `6e400002-…` |
| Conn params | board requests 15–30 ms interval; MCUmgr forces fast params during SMP |
| PHY / MTU | 2M PHY requested on connect; DLE 251; L2CAP MTU 498 (SMP); ATT MTU ≥207 needed for DATA frames |
| Advertising | fast (30–60 ms) for 30 s after boot/disconnect, then slow (~1 s) |
| Concurrency | one central per board |

All frames start with magic `0xC9A5` (u16 LE). One frame per
notification. Little-endian throughout.

## DATA frame — type 0x11, 204 B, 25/s

Batches 4 sensor ticks (10 ms each).

| Off | Size | Field |
|---|---|---|
| 0 | u16 | magic |
| 2 | u8 | type 0x11 |
| 3 | u8 | seq (shared counter with STATUS/TSYNC, wraps) |
| 4 | u32 | t_ms — device uptime of the LAST tick (sample k of n is at `t_ms − (n−1−k)·10`) |
| 8 | u8 | ppg_valid_mask (bit N = site N+1 live) |
| 9 | u8 | n_samples (=4) |
| 10 | u8 | baro_ok_mask |
| 11 | u8 | reserved |
| 12 | 144 B | PPG, sensor-major: site 0..3 × sample 0..3 × (R,IR,G) u24. Red of (s,k) at `12 + s·36 + k·9` |
| 156 | 48 B | baro, sample-major: sample 0..3 × baro 0..3, u24 Pa. (k,b) at `156 + k·12 + b·3`. Baro runs 25 Hz so the 4 in-frame samples may repeat |
| 204 | | total |

## STATUS frame — type 0x12, 45 B, 1/s

| Off | Size | Field |
|---|---|---|
| 8 | 12 B | 3 × { i16 SHT temp ×0.01 °C, u16 SHT RH ×0.01 % } |
| 20 | 6 B | 3 × i16 TMP117 skin temp ×0.01 °C |
| 26 | 8 B | 4 × i16 baro temp ×0.01 °C |
| 34 | u16 | vbat_mv |
| 36 | u8 | flags: bit0 mask present, bit1 SD logging active, **bit2 sensing active** |
| 37 | u8 | achieved PPG rate (Hz) |
| 38 | u8 | achieved baro rate |
| 39–42 | u8×4 | ppg/baro/sht/tmp validity masks |
| 43 | u8 | BLE frames shed last second (saturating) |
| 44 | u8 | AIMD decimation (board sends every Nth DATA frame) |

Hosts should accept 43 B frames from older builds (no link bytes).

## TSYNC record — type 0x13, 16 B — SD log only

`u32 uptime_ms` @4, `u64 epoch_ms` @8. Written once per second once
the clock is known. Never sent over BLE/RTT; hosts need not parse it.

## RX commands

| Cmd | Payload | Effect |
|---|---|---|
| `'B'` 0x42 | — | binary streaming (default; send on connect) |
| `'J'` 0x4A | — | 1 Hz JSON debug line instead of frames |
| `'T'` 0x54 | u64 LE epoch ms | wall-clock sync. Send on every connect, optionally every ~10 min. One sync retroactively timestamps the whole boot's SD log (uptime is monotonic); the board has no crystal RTC (~0.1 % drift), so repeats improve accuracy |
| `'P'` 0x50 | u8 0/1 | remote sensing off/on. OFF = standby power state (PPG LEDs off, no DATA; STATUS keeps flowing). Survives disconnect; boot default ON. Actual state = STATUS flags bit2 |

Portal connect sequence: subscribe TX → write `'B'` → write `'T'`+epoch.

## Link behavior

- Board sheds the OLDEST queued frame under congestion (newest data
  always flows); shed frames appear as device-time jumps — break chart
  traces there, do not bridge.
- AIMD pacing: sustained shed → board halves DATA rate (up to 8×),
  probes back after calm; live values in STATUS bytes 43/44.
- Plot against `t_ms` (device timebase), never arrival time.

## Side services

- **OTA DFU**: MCUmgr SMP service (UUID `8D53DC1D-…`), MCUboot
  overwrite-only, dev-key signed. `python scripts/cpap_ctl.py --dfu
  build/Moamoa_CPAP_PI_firmware/zephyr/zephyr.signed.bin`. See
  `build-flash-debug.md`.
- **Log fetch**: MCUmgr FS download, paths `/SD:/LOG/INDEX.TXT` and
  `/SD:/LOG/BnnnSmmm.BIN` (`cpap_ctl.py --fetch`). ~18 KiB/s.
- **Throughput sink**: service 0xFFE0, char 0xFFE7 — writes counted
  and discarded, read returns u32 LE counter (`cpap_ctl.py --tput`).
