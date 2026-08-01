# CLAUDE.md — Agent Coding Instructions

> Full docs in docs/ (architecture, ble-protocol, sd-logging,
> build-flash-debug, hardware, power, portal-integration). This file is
> the compact reference for coding.

## Quick Ref

- **SDK**: nRF Connect SDK v3.3.0 / Zephyr v4.3.99 (sysbuild + MCUboot)
- **Module**: Raytac MDBT50Q-P1MV2 (nRF52840), normal-voltage mode (VDD=VDDH=3.3 V)
- **Board**: `cpap_pi_control/nrf52840` (custom, in `Boards/kamoamoa/cpap_pi_control/`)
- **Console**: RTT only (`printk()`), no UART/USB. No `%f` — fixed-point ints.
- **Ninja**: `C:\Users\xwang3239\ncs\toolchains\936afb6332\opt\bin\ninja.exe`

## Commands

One-shot build (pristine needs `-DBOARD_ROOT=.` so sysbuild finds the board):
`nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- west build -b cpap_pi_control/nrf52840 -- -DBOARD_ROOT=.`
Flash: `west flash` (routine; `--recover` only for locked chips — wipes UICR)
OTA: `python scripts/cpap_ctl.py --dfu build/Moamoa_CPAP_PI_firmware/zephyr/zephyr.signed.bin` (bump `VERSION` first)
RTT console (5 s timeout; empty = no output):
`& "~\Downloads\SimplicityCommander-Windows\SimplicityCommander-Windows\Commander-cli_win32_x64_1v24p1b1980\Simplicity Commander CLI\commander-cli.exe" rtt connect --device nrf52840_xxaa`

## Code map

- `src/sensors/sensor_manager.c` — THE 10 ms tick thread (absolute deadlines): PPG ~97 Hz ×4, baro 25 Hz ×4, SHT/TMP/batt/presence 1 Hz, STANDBY state machine (mask absent 5 s or 'P' off → PPG SHDN + pause)
- `src/comm/comm_protocol.h` — wire format v2.1, single source of truth; portal useComm.js + scripts/rtt_bridge.py FRAME_LEN + tools/sd_reader.py MUST match
- `src/comm/comm_manager.c` — frame build, fan-out SD/RTT/BLE, AIMD pacing, RX commands ('B'/'J'/'T' timesync/'P' sensing on-off)
- `src/comm/ble_manager.c` — NUS, adv backoff (fast 30 s → slow), 2M PHY, TX credits
- `src/storage/sd_logger.c` — SPSC ring → writer thread → rotating FAT files + TSYNC wall-clock records
- `src/drivers/driver_ms5611.c` — MS5611 **I2C** mode ×4, MS5611 math (NOT ms5607 exponents)
- `tools/sd_reader.py` — parser + doctor GUI; rebuild exe via tools/make_exe.bat; ALWAYS run `--selftest` after changes

## House rules

- Nothing in the sensor tick or a BLE callback may block; only the
  comm_tx thread sends notifications; SD writes only via the ring.
- New periodic work: add a tick phase in sensor_manager, not a timer;
  respect `standby`.
- Subsystem Kconfigs (I2C/SPI/ADC) go in prj.conf, NOT the board
  defconfig (MCUboot child image can't build SAADC).
- i2c0 stays at 400 kHz (100 kHz silently breaks the 100 Hz loop).
- Frame format changes: update comm_protocol.h + portal + rtt_bridge +
  sd_reader together, and docs/ble-protocol.md.
- One debug tool at a time on the J-Link; check VTref with probe idle
  before believing a board is powered (phantom-power trap).
- Verify on hardware before committing; commit style: what + why +
  what was verified.

## Current hardware quirks

- 5 V boost has no EN gating this rev (always on; fixed next rev).
- Debug LEDs DNP. SD "init failed (-134)" = no card inserted.
- Windows BLE: retry truncated discovery; board reset clears zombie
  connections; bleak needs use_cached_services=False.
