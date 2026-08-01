# System Architecture

CPAP-PI is a two-board wearable that measures mask-to-skin interaction
during CPAP therapy: pulse oximetry (PPG), contact pressure, skin
temperature and mask micro-climate at four mask sites, streamed live to
a web/tablet portal over BLE and simultaneously logged to a microSD
card with real-world timestamps.

## The pieces

| Piece | Where | What it is |
|---|---|---|
| Control board | `Moamoa_CPAP_PI_hardware/kmm-pmask-control` | nRF52840 (Raytac MDBT50Q-P1MV2), battery power chain, microSD, SWD, 30-pin FFC to the mask |
| Mask flex PCB | `Moamoa_CPAP_PI_hardware/kmm-pmask-mask` | 14 sensors in 4 site clusters behind a TCA9546A I2C mux |
| **This firmware** | `Moamoa_CPAP_PI_firmware` | Zephyr / nRF Connect SDK v3.3.0, board `cpap_pi_control/nrf52840` |
| Web portal | `../CPAP_PI_portal` | React + Web Bluetooth, parses the binary stream (also an Android tablet app branch) |
| Doctor's SD reader | `tools/` → `dist/CPAP_PI_SD_Reader.exe` | Zero-config Windows viewer/exporter for the SD card |

## Firmware component map

```
src/
├── main.c                 boot: LEDs, bus diagnostics, comm init, SD logger,
│                          sensor thread; then 0.5 Hz heartbeat
├── drivers/
│   ├── bus_diag.c         boot-time scan of every mux channel + MS5611 PROM
│   │                      CRCs + SD probe; pulsed mask-presence sampling
│   ├── driver_ms5611.c    4x MS5611 in I2C mode: concurrent conversions,
│   │                      MS5611 compensation math (NOT the ms5607 exponents)
│   ├── driver_batt.c      VBAT via AIN7 (1M/1M divider)
│   └── driver_led.c       4 debug LEDs (DNP on current boards)
├── sensors/
│   ├── sensor_manager.c   THE 10 ms tick thread (see below)
│   ├── ppg_reader.c       4x MAX30101 via in-tree driver + SHDN control
│   ├── sht40_reader.c     3x SHT40 via in-tree driver (staggered 1 Hz)
│   └── tmp117_reader.c    3x TMP117, one-shot mode (trigger→read split)
├── comm/
│   ├── comm_protocol.h    THE wire format (single source of truth — the
│   │                      portal parser, rtt_bridge.py and sd_reader.py
│   │                      must all match this file)
│   ├── comm_manager.c     tick batching → frames; fan-out to SD/RTT/BLE;
│   │                      AIMD link pacing; RX command handling
│   ├── ble_manager.c      NUS transport, advertising backoff, 2M PHY,
│   │                      conn params, TX credits plumbing
│   ├── rtt_stream.c       same frames on RTT up-buffer 1 (wired debug)
│   └── tput_sink.c        0xFFE7 byte-sink for link measurements
└── storage/
    └── sd_logger.c        FAT logging: SPSC ring → writer thread → rotating
                           files; INDEX.TXT; circular deletion; hot-plug
```

## Threads and priorities

| Thread | Prio | Role | Never does |
|---|---|---|---|
| `sensors` | 5 (preempt) | 10 ms absolute-deadline tick: all sensor I/O, frame building, ring pushes | block on radio or SD |
| `comm_tx` | 7 | BLE sends, credit-paced (≤2 in flight, 300 ms shed timeout) | touch sensors |
| `sd_writer` | 9 | drains the 32 KB ring in ≥4 KB batches, fsync 2 s, rotation, hot-plug | run in the tick path |
| MCUmgr WQ | (stack 8 KB) | OTA/SMP + file downloads | — |

House rules (inherited from the caterpillar project, learned the hard
way): nothing in the sensor tick or a BLE callback may block; only
`comm_tx` sends notifications; flash/SD writes live behind a RAM ring.

## Data flow

```
                    10 ms tick (sensor_manager)
 4x MAX30101 ──I2C──┐
 4x MS5611  ──I2C──┤ per tick: sample → tick_sample
 3x SHT40   ──I2C──┤ 1 Hz: SHT/TMP/batt/presence → g_sensor_data
 3x TMP117  ──I2C──┘
                     │ every 4 ticks: build 204 B DATA frame
                     │ every 1 s:     build 45 B STATUS (+16 B TSYNC)
                     ▼
        ┌────────────┼──────────────┐
        ▼            ▼              ▼
   SD ring (all) RTT ch1 (1/3)  BLE queue (AIMD-paced)
        ▼            ▼              ▼
   sd_writer     rtt_bridge.py   comm_tx thread
   FAT files     ws://:8765      NUS notifications
        ▼            ▼              ▼
   SD card       portal (wired)  portal / tablet (25 fps)
```

The same binary frames travel all three paths — one parser everywhere.

## Power states

- **ACTIVE**: everything running (~10–12 mA). PPG 4×~97 Hz, baro 4×25 Hz,
  SHT/TMP 1 Hz.
- **STANDBY**: entered when the mask is absent ≥5 s OR the portal sends
  `'P'`+0. PPGs in SHDN (LED drive off), sampling paused, no DATA
  frames; presence + battery + STATUS continue (~1–2 mA). Exits on mask
  attach + remote-enable.

See `power.md` for the full budget and history.

## Related repos & history

- `../CPAP_PI_firmware` — the **rev1** system (3 PPG + FSR + 6 SPI baro,
  protocol v1). Incompatible wire format; kept for reference.
- `../Moamoa_caterpillar_firmware` — sibling project; source of the OTA
  stack, BLE tuning, and most architectural house rules.
- The `eval` branch of `CPAP_PI_firmware` — the original nRF52840-DK
  proof of concept.
