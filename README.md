# CPAP-PI Control Firmware

Firmware for the CPAP-PI mask-sensing system: an nRF52840 control
board driving a 14-sensor mask flex PCB (4 sites × PPG + contact
pressure, 3× temp/humidity, 3× skin temp), streaming live over BLE to
a web/tablet portal and logging everything to microSD with real-world
timestamps. OTA-updatable in the field.

- SDK: **nRF Connect SDK v3.3.0** (Zephyr 4.3.99), sysbuild + MCUboot
- Board: `cpap_pi_control/nrf52840` (custom, `Boards/kamoamoa/…`)
- Console: RTT only (no UART/USB)

## Documentation (start here)

| Doc | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | system pieces, firmware map, threads, data flow, power states |
| [docs/ble-protocol.md](docs/ble-protocol.md) | wire format v2.1, commands, OTA/fetch/throughput services |
| [docs/portal-integration.md](docs/portal-integration.md) | portal/tablet developer cheat sheet (no hardware knowledge needed) |
| [docs/sd-logging.md](docs/sd-logging.md) | card format, wall-clock/TSYNC system, doctor's reader exe |
| [docs/build-flash-debug.md](docs/build-flash-debug.md) | build/flash/OTA commands, RTT, debug-probe field guide |
| [docs/hardware.md](docs/hardware.md) | pin maps, mask topology, electrical facts, fault signatures |
| [docs/power.md](docs/power.md) | power budget, optimizations, remaining levers |

## Quick start

```
# build (pristine; later builds: plain `west build`)
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- ^
  west build -b cpap_pi_control/nrf52840 -- -DBOARD_ROOT=.

west flash                      # first install (SWD, keeps UICR)
python scripts/cpap_ctl.py --dfu build/Moamoa_CPAP_PI_firmware/zephyr/zephyr.signed.bin   # updates (OTA)
```

Watch it run: `commander-cli rtt connect --device nrf52840_xxaa` —
1 Hz summary of every sensor, rates, battery, mask state.

## Tooling

- `scripts/cpap_ctl.py` — `--tput` link test, `--dfu` OTA, `--sync`
  wall clock, `--fetch` SD log download over BLE
- `scripts/ble_test.py` — 20 s live-stream health check
- `scripts/rtt_bridge.py` — wired stream → `ws://localhost:8765`
- `tools/` — doctor-facing SD reader (`make_exe.bat` →
  `dist/CPAP_PI_SD_Reader.exe`)

## Companions

- Web portal: `../CPAP_PI_portal` (protocol v2 parser in `src/useComm.js`)
- Hardware (KiCad): `../Moamoa_CPAP_PI_hardware`
- Rev1 system (incompatible v1 protocol, reference only): `../CPAP_PI_firmware`
