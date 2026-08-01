# Build, Flash, OTA & Debug

## Build

```
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- ^
  west build -b cpap_pi_control/nrf52840 -- -DBOARD_ROOT=.
```

- SDK: nRF Connect SDK **v3.3.0** (Zephyr 4.3.99). `-DBOARD_ROOT=.` is
  required on the first/pristine configure (sysbuild doesn't see the
  app CMakeLists' BOARD_ROOT append); plain `west build` after that.
- Sysbuild + MCUboot: outputs in `build/` — `merged.hex` (MCUboot+app,
  for SWD) and `build/Moamoa_CPAP_PI_firmware/zephyr/zephyr.signed.bin`
  (for OTA). Images auto-sign with the SDK dev key ("not secure"
  warning is expected — intentionally open research device).
- Bump `VERSION` before OTA releases; MCUboot prints it at boot.
- Board subsystem configs (I2C/SPI/ADC) live in **prj.conf**, not the
  board defconfig — the MCUboot child image builds against the same
  board and its no-multithreading environment can't compile SAADC.

## Flash (SWD)

```
west flash              # routine — preserves UICR and SD/settings
west flash --recover    # ONLY for locked/wedged chips (mass erase)
```

Needed once to install MCUboot; afterwards prefer OTA. This board runs
normal-voltage mode (VDD=VDDH=3.3 V) so the rev1 REGOUT0 saga does not
apply here.

## OTA (BLE)

```
python scripts/cpap_ctl.py --dfu build/Moamoa_CPAP_PI_firmware/zephyr/zephyr.signed.bin
```

Upload ~11 KiB/s → mark → auto-reboot → overwrite-install (~10 s radio
silence). Same-image re-upload is rejected harmlessly. Other
`cpap_ctl.py` functions: `--tput [KiB]`, `--sync`, `--fetch <file|INDEX>`.

## Console & wired stream (RTT)

```
commander-cli rtt connect --device nrf52840_xxaa      # console (ch 0)
python scripts/rtt_bridge.py                          # data (ch 1) → ws://localhost:8765
```

- The board has **no UART/USB** — RTT is the only wired I/O.
- The 1 Hz console summary shows every sensor, achieved rates, VBAT,
  mask state — first stop for any "is it working" question.
- Bridge caveat: the J-Link EDU Mini drains RTT at only ~2.3 kB/s, so
  the wired DATA stream is decimated to every 3rd frame (`RTT_DECIM`).
  BLE carries full rate. A J-Link BASE+ lifts this.

## Debug-probe field guide (hard-won)

1. **One debug tool at a time.** commander-cli, nrfutil, pylink and the
   bridge all fight over the probe; overlapping sessions produce bogus
   timeouts and "DLL error -2" that mimic a dead/locked chip.
2. **Phantom power**: an *unpowered* board back-fed through SWD reads
   VTref ≈ 3 V and even answers memory reads — but mass erase fails
   with LOW_VOLTAGE. Check VTref with the probe **idle** (pylink
   `hardware_status.voltage`) before trusting anything.
3. **"No probe can connect but firmware runs"** on some nRF52840s =
   hardened-APPROTECT silicon flashed by a raw J-Link tool that didn't
   rewrite `UICR.APPROTECT=0x5A`. `west flash --recover` fixes it.
   (This board's chip is not hardened, but collaborators' boards vary —
   the rev1 firmware carries a self-heal hook for reference.)
4. RTT attach can race a just-reset target and bind a stale control
   block — if the bridge shows a garbage buffer name, restart it.
5. Windows BLE: truncated GATT discovery happens — retry the connect;
   zombie connections after killed scripts — reset the board.
   `use_cached_services=False` on bleak/WinRT.

## Verifying a change

- `scripts/ble_test.py` — 20 s live stream: fps, kB/s, link health.
- `scripts/cpap_ctl.py --tput 64` — raw link throughput (expect ~20 KiB/s).
- RTT summary — sensor rates (expect ~97/25 Hz).
- `tools/sd_reader.py --selftest <fetched.bin>` — end-to-end log check.
