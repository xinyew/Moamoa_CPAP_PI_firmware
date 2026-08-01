# Portal / Tablet Integration Cheat Sheet

For the web-portal or Android developer. No hardware knowledge needed.
Everything below runs over Web Bluetooth / any BLE stack.

## Connect

1. Scan for name prefix `CPAP` → `CPAP_PI_Control`.
2. Connect GATT, get NUS service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`.
3. Subscribe to TX `6e400003-…` (notifications = binary frames).
4. Write to RX `6e400002-…`:
   ```js
   await rx.writeValueWithoutResponse(new Uint8Array([0x42]));   // 'B' binary mode
   const t = new DataView(new ArrayBuffer(9));
   t.setUint8(0, 0x54);                                          // 'T' time sync
   t.setBigUint64(1, BigInt(Date.now()), true);
   await rx.writeValueWithoutResponse(t.buffer);
   ```
   The time sync timestamps the device's SD log — send it on EVERY
   connect (and optionally every ~10 min).

## Receive

Parse per `docs/ble-protocol.md` (DATA 0x11 204 B @25/s, STATUS 0x12
45 B @1/s). Plot against the in-frame device time `t_ms`, not arrival
time. Break traces across device-time jumps (those are shed frames).

Useful STATUS fields for UI:
- flags bit0 = mask attached, bit1 = SD logging, bit2 = sensing on
- bytes 37/38 = achieved sensor rates; 43/44 = link drops + pacing
- vbat_mv @34 for a battery gauge

## Control

```js
// sensing off (power save when mask idle) / on
await rx.writeValueWithoutResponse(new Uint8Array([0x50, 0]));  // 'P' off
await rx.writeValueWithoutResponse(new Uint8Array([0x50, 1]));  // 'P' on
```
Reflect the *device-reported* state (STATUS flags bit2) in the UI, not
the last command sent. While off: DATA stops, STATUS continues — show
"sensing paused", not a frozen screen. State survives disconnects;
reboot restores ON.

## Behavior to expect

- Fresh boot / just disconnected: board is discoverable in <1 s.
  After 30 s idle it advertises slowly — discovery may take ~1–2 s.
- Mask unplugged: after 5 s the board pauses sensing itself (STATUS
  bit0 and bit2 clear). It resumes automatically on reattach unless
  `'P'` off is in effect.
- Under a weak link the board thins the DATA rate (STATUS byte 44 >1)
  rather than stalling; full-rate data is always on the SD card.

## Windows quirks (from testing)

- Occasional truncated GATT discovery → retry the connect.
- After killing a script mid-connection the board may look
  undiscoverable (zombie link) → reset the board or wait ~30 s.
- bleak/WinRT: pass `use_cached_services=False`; connect using the
  scanner's device object, not a bare address string.
