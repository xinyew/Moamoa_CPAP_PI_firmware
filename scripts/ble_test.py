#!/usr/bin/env python3
"""
Live BLE throughput / link-health test for the KMM PMask firmware.

Connects to "KMM_PMask_Control", subscribes to the NUS stream and
reports: DATA fps (target 25), total kB/s, and the in-band link
health from the STATUS frame (drops/s, AIMD decimation, sensor
rates, battery, validity masks).

Usage:  python ble_test.py [--seconds 20]
Requires: pip install bleak
"""

import argparse
import asyncio
import struct
import time

from bleak import BleakClient, BleakScanner

NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

stats = {"data": 0, "status": 0, "bytes": 0, "t0": None, "last_status": None}


def on_notify(_, payload: bytearray):
    if len(payload) >= 4 and struct.unpack_from("<H", payload, 0)[0] == 0xC9A5:
        if stats["t0"] is None:
            stats["t0"] = time.time()
        stats["bytes"] += len(payload)
        if payload[2] == 0x11 and len(payload) == 204:
            stats["data"] += 1
        elif payload[2] == 0x12 and len(payload) == 45:
            stats["status"] += 1
            stats["last_status"] = bytes(payload)


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=int, default=20)
    args = parser.parse_args()

    print("scanning...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: d.name and d.name.startswith("KMM"), timeout=15.0)
    if dev is None:
        print("KMM_PMask_Control not found (powered? advertising?)")
        return

    print(f"found {dev.name} [{dev.address}]")
    async with BleakClient(dev) as client:
        print(f"connected, MTU={client.mtu_size}")
        await client.start_notify(NUS_TX, on_notify)
        await client.write_gatt_char(NUS_RX, b"B", response=False)
        await asyncio.sleep(args.seconds)
        await client.stop_notify(NUS_TX)

    dt = time.time() - stats["t0"] if stats["t0"] else 1
    print(f"\nDATA:   {stats['data']} in {dt:.1f}s = "
          f"{stats['data']/dt:.1f} fps (target 25)")
    print(f"STATUS: {stats['status']} ({stats['status']/dt:.1f}/s), "
          f"total {stats['bytes']/dt/1024:.2f} kB/s")
    s = stats["last_status"]
    if s:
        vbat = struct.unpack_from("<H", s, 34)[0]
        print(f"link:   drops/s={s[43]} decim=x{s[44]} "
              f"sensor rates={s[37]}/{s[38]}Hz vbat={vbat}mV "
              f"masks ppg=0x{s[39]:X} baro=0x{s[40]:X} "
              f"sht=0x{s[41]:X} tmp=0x{s[42]:X}")


if __name__ == "__main__":
    asyncio.run(main())
