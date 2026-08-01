# Power Management

## Budget (datasheet arithmetic; battery-side unless noted)

| Consumer | Before optimization | Now (ACTIVE) | Now (STANDBY) |
|---|---|---|---|
| PPG LED drive (4× MAX30101, 3 LEDs, 411 µs @ ~97 Hz) | ~6–9 mA | ~6–9 mA | ~0 (SHDN) |
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

- PPG pulse width 411 → 215 µs: halves the dominant LED energy for one
  bit of ADC resolution. Do this first if more life is needed.
- PPG site duty-cycling (e.g. 10 s/min spot checks): order-of-magnitude,
  needs a clinical decision.
- Next hardware rev: 5 V boost EN on a GPIO (kills the last always-on
  in STANDBY); rail LEDs already DNP.

## Rules for future code

- New periodic work goes in the sensor tick phases, not its own timer.
- Anything that touches the mask sensors must respect `standby` (see
  `sensor_manager.c`) — check `g_sensor_data.sensing_on`.
- Sleep is Zephyr idle — never busy-wait; blocking driver calls (I2C
  DMA) already let the CPU sleep.
