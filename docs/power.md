# Power Management

## 2026-09 direction change: PPG is performance-first, not power-first

The rest of this doc (and the levers below) predate a project decision
to prioritize PPG signal quality over continuous-run power budget —
intermittent (burst) sensing will eventually carry the power savings
instead of a lean continuous mode. As of this pass, LED current has
been raised well above the "Now (ACTIVE)" row below (see
`docs/hardware.md`); the budget table's PPG row and totals are stale
until re-measured. Intermittent-sensing scheduling itself is not yet
implemented — see "Remaining levers" below, now the top item.

## Budget (datasheet arithmetic; battery-side unless noted) — PPG row stale, see above

| Consumer | Before optimization | Now (ACTIVE) | Now (STANDBY) |
|---|---|---|---|
| PPG LED drive (4× MAX30101, 3 LEDs, 411 µs @ ~97 Hz) | ~6–9 mA | ~6–9 mA (stale — raised 2026-09, not remeasured) | ~0 (SHDN) |
| MS5611 conversions | ~1.3 mA @50 Hz | ~0.6 mA @25 Hz | 0 |
| TMP117 ×3 | ~0.6 mA continuous | ~µA (one-shot) | 0 |
| Presence pull-ups | ~0.5 mA (mask on) | ~0 (pulsed 100 µs/s) | ~0 |
| BLE advertising (unconnected) | ~0.3–0.5 mA | same 30 s, then ~30 µA | ~30 µA |
| CPU + buses + SD avg | ~1.5–2.5 mA | ~1.5–2.5 mA | ~0.5–1 mA |
| 5 V boost quiescent | always | always | always (HW: no EN this rev) |
| **Total** | **~12–15 mA** | **~10–12 mA** | **~1–2 mA** |

Rough life on the current pack: active nights ~unchanged order, standby
~a week instead of ~a day. These are estimates — a Power Profiler Kit II
inline with the battery is the way to get real numbers.

## Implemented (commits `ffba455`…`4c03bf3`)

1. **Pulsed presence sampling** — PRESEN pins float except a 100 µs
   pulled-up window at 1 Hz (`bus_diag_sample_presence`).
2. **Baro 25 Hz** (was 50) at unchanged OSR 2048.
3. **TMP117 one-shot** — shutdown at init; trigger at tick phase 40,
   read at phase 70 (124 ms > conversion), auto-shutdown after.
4. **Advertising backoff** — fast 30 s after boot/disconnect, then
   ~1 s interval. Discovery measured 0.2 s fast / 1.3 s slow.
5. **STANDBY state** — entered on mask absent ≥5 s (hysteresis) or
   `'P'`+0 (instant). PPGs → SHDN via MODE_CFG bit (registers retained,
   wake resumes DT config), baro/SHT/TMP paused, DATA frames stop.
   Presence, battery, STATUS (with state bits) and SD STATUS logging
   continue. Exit: mask present AND remote-enabled.

## Non-issues (measured/reasoned — don't "optimize" these)

- **RTT streaming**: memcpy into RAM, skip-if-full; costs ~nothing and
  is the field diagnostic channel. Keep it.
- Debug LED GPIO toggling: LEDs are DNP; driving the pins is ~0.
- SD write cadence: already batched (4 KB/1.5 s); stretching it trades
  power-loss window for marginal gain.

## Remaining levers (not implemented)

- **PPG intermittent sensing (e.g. 10 s on / 10 min off)** — now the
  primary lever, not optional: this is what will make the raised LED
  current affordable on average. Deliberately deferred as a separate
  pass (state machine in `sensor_manager.c`, composes with the
  existing mask-presence STANDBY logic). Not implemented yet.
- PPG pulse width 411 µs: intentionally left at the max-resolution
  setting (opposite of the old power-saving direction — see above).
- Next hardware rev: 5 V boost EN on a GPIO (kills the last always-on
  in STANDBY); rail LEDs already DNP.

## Implemented, performance-first pass (2026-09)

- **LED current raised**: R/IR 6.2→~19.2 mA, G 25.4→~35.2 mA
  (`led-pa` in the DTS), still below the saturating 0xFF default.
- **Ambient/dark baseline subtraction** (`ppg_reader.c`): LEDs forced
  off once at init, one FIFO sample captured per sensor as a dark
  reference, subtracted from every subsequent reading. Recovers the
  ADC headroom the higher LED current spends, and removes ambient
  room/sunlight DC offset from the signal. Captured once at boot only
  — not re-calibrated periodically (that's tied to the deferred
  intermittent-sensing work, where each burst would plausibly want a
  fresh baseline after the sleep gap).
- **`smp-ave = <4>` — tried and reverted (2026-09-06).** The binding
  says on-chip averaging "can be averaged and **decimated**" — the
  decimation is the part that matters: it drops the FIFO's true
  refresh rate to `smp-sr/smp-ave` (25 Hz at 4x), it does *not* keep
  100 Hz output with free noise reduction as originally assumed here.
  Our 100 Hz tick still polls every 10 ms regardless, so 3 of every 4
  polls just re-read the same not-yet-updated FIFO entry (confirmed:
  MAX30101 returns the last valid entry, unadvanced, when read faster
  than new data arrives). Showed up as an exact repeat-3x-then-jump
  stairstep in the logged waveform. Left at default (1, no averaging)
  so every tick gets an independent fresh sample. If real SNR gain
  from averaging is wanted later, it has to come with either dropping
  the tick's PPG poll rate to match `smp-sr/smp-ave`, or upping
  `smp-sr` proportionally so the effective refresh rate stays 100 Hz
  (e.g. `smp-sr=400`, `smp-ave=4` — unverified, would need the same
  bench check).
- **IR slot duplication — tried and reverted (2026-09-06).**
  `led-slot = <1 2 3 2>` (repeating IR into the previously-idle 4th
  slot) causes a real memory-safety bug in this NCS driver
  (`zephyr/drivers/sensor/maxim/max30101/max30101.h`):
  `MAX30101_MAX_NUM_CHANNELS` is hardcoded to `3` — the FIFO read
  buffer (9 bytes) and `raw[]` array (3 elements) are sized for at
  most 3 *active slots total*, not 3 distinct LEDs. Enabling a 4th
  slot makes `total_channels = 4`, so `sample_fetch()` reads 12 bytes
  into the 9-byte buffer and writes `raw[3]` past the array — which
  overlaps `map[][]`, corrupting the channel-to-slot mapping on every
  fetch. Manifested as PPG values freezing for ~1.2 s at a stretch
  (confirmed via a captured CSV: device-side `t_ms` was gapless, so
  frames were genuinely arriving — the payload itself was corrupted/
  stale). **Do not enable all 4 slots on this driver version** for any
  reason (duplication or otherwise) unless the driver's channel
  buffers are patched to size off `total_channels` instead of the
  hardcoded constant.

## Rules for future code

- New periodic work goes in the sensor tick phases, not its own timer.
- Anything that touches the mask sensors must respect `standby` (see
  `sensor_manager.c`) — check `g_sensor_data.sensing_on`.
- Sleep is Zephyr idle — never busy-wait; blocking driver calls (I2C
  DMA) already let the CPU sleep.
