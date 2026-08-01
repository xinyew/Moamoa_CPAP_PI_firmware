# Hardware Notes (firmware-facing)

Schematics: `Downloads/Moamoa_CPAP_PI_hardware/` — `kmm-pmask-control`
(hierarchical: MCU / power / perhipherals / ext_conn sheets) and
`kmm-pmask-mask`. (The KiCad project names predate the CPAP-PI rename.)

## Control board pin map (nRF52840, MDBT50Q-P1MV2)

| Pin | Function | Notes |
|---|---|---|
| P0.13 / P0.15 | I2C SDA / SCL | to mask via PCA9517A repeater, 400 kHz |
| P0.14 | PCA9517A EN | must be HIGH before any mask I2C |
| P0.16 | mask mux ~RST | TCA9546A reset, active low |
| P0.30 / P0.28 | PRESEN A / B | grounded by the mask; sampled with pulsed pull-ups |
| P0.12 / P0.11 / P1.09 / P0.07 | SD CS / SCK / MOSI / MISO | microSD, SPI mode, spi1 @8 MHz |
| P0.31 | AIN7 battery sense | VBAT/2 via 1M/1M |
| P0.19 / P0.21 / P0.20 / P0.22 | debug LEDs 0-3 | active low, **DNP on current boards** |
| P0.18 | ~RESET | button + SWD |

Power: battery → slide switch SW1 → P-FET → {TPS61202 5 V boost
(PPG LEDs), AP2112K 3.3 V, AP2112K 1.8 V}. VDD=VDDH=3.3 V
(normal-voltage mode — no REGOUT0/UICR dependence). No 32 kHz crystal:
`K32SRC_RC` with periodic calibration.

## Mask flex (via symmetric 30-pin FFC)

- FFC is **orientation-proof**: signals mirrored, power symmetric, both
  PRESEN pins grounded mask-side.
- TCA9546A mux @0x70 (A0=A1=GND), 4 downstream channels = 4 sites:

| Channel | Sensors (7-bit addr) |
|---|---|
| ch0 | MAX30101 0x57, MS5611 0x77, SHT40 0x44, TMP117 0x48 |
| ch1 | MAX30101 0x57, MS5611 0x77 |
| ch2 | MAX30101 0x57, MS5611 0x77, SHT40 0x44, TMP117 0x48 |
| ch3 | MAX30101 0x57, MS5611 0x77, SHT40 0x44, TMP117 0x48 |

- MS5611s are in **I2C mode** here (PS=3.3 V, CSB=GND → 0x77) — unlike
  rev1's SPI wiring. 14 sensors total.
- Per-channel bus pull-ups + test points TP1–TP8 on the mask.
- MAX30101 rails: core 1.8 V, LEDs 5 V, both from the control board.

## Firmware-relevant electrical facts

- i2c0 must run at **400 kHz** (`I2C_BITRATE_FAST` in the board DTS):
  ~26 mux-switched transactions per 10 ms tick don't fit at 100 kHz —
  this once caused a hidden 46 Hz sampling loop.
- MAX30101 LED currents are set in the DTS (`led-pa`): R/IR 6.2 mA,
  G 25.4 mA, ADC range 16384 — the 51 mA default saturates on skin.
- The 5 V boost has **no enable control** on this rev (EN tied to
  battery) — its quiescent draw exists whenever the switch is on.
  Next rev: EN from a GPIO; also planned: VTref sensing the true VDD.

## Diagnosing sensor failures

Boot diagnostics print per-channel findings. Patterns seen in practice:
- One MAX30101 missing, channel partners alive → that chip's own
  SDA/SCL/VDD castellation joints (OESIP side-wettable pads are the
  hardest hand-solder part on the mask).
- All PPGs missing, 3.3 V sensors fine → the single 1.8 V FFC pin.
- A contiguous group dead → FFC seating (reseat, re-check; the scan
  reruns every boot and absent sensors rejoin automatically).
- PPG present but dark/zero → 5 V LED rail.
- "SD init failed (-134)" → empty socket (or, with a card: socket
  joints — the one connector never exercised by the sensor paths).
