#!/usr/bin/env python3
"""
KMM PMask BLE utility — throughput test and OTA DFU.

Two functions (ported from the caterpillar project's field-proven
tooling; same firmware wire contracts):

  Throughput test — floods the 0xFFE7 byte-sink characteristic with
  GATT writes and compares the device-side counter with what was
  sent (loss check) plus the measured KiB/s.

  OTA DFU — uploads a signed MCUboot image over MCUmgr SMP (BLE),
  marks it for install and reboots. The device overwrite-installs it
  on boot (~10 s of radio silence during the copy).

Usage:
    python kmm_ctl.py --tput            # throughput test, default 64 KiB
    python kmm_ctl.py --tput 256        # ... custom amount
    python kmm_ctl.py --dfu build/Moamoa_CPAP_PI_firmware/zephyr/zephyr.signed.bin
    python kmm_ctl.py --name KMM_PMask_Control   # optional device filter

Requires: pip install bleak            (throughput)
          pip install smpclient        (DFU)
"""

import argparse
import asyncio
import struct
import sys

from bleak import BleakClient, BleakScanner

DEVICE_PREFIX = "KMM"
CHAR_UUID_TPUT = "0000ffe7-0000-1000-8000-00805f9b34fb"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"


async def discover(name_prefix: str):
    print(f"Scanning for \"{name_prefix}*\" ...", flush=True)
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: d.name and d.name.startswith(name_prefix),
        timeout=15.0)
    if dev is None:
        print(f"No device matching \"{name_prefix}*\" found.",
              file=sys.stderr)
        sys.exit(1)
    print(f"Found {dev.name} [{dev.address}]")
    return dev


async def tput_test(device, kib: int):
    """Measure raw GATT write throughput against the 0xFFE7 sink."""
    total = kib * 1024
    print(f"Connecting to {device.address} ...", flush=True)
    async with BleakClient(device,
                           winrt=dict(use_cached_services=False)) as client:
        mtu = client.mtu_size
        chunk = max(20, mtu - 3)
        print(f"Connected (MTU={mtu}), chunk {chunk} B, sending {kib} KiB ...")

        c0 = struct.unpack("<I",
                           await client.read_gatt_char(CHAR_UUID_TPUT))[0]
        payload = bytes(chunk)
        loop = asyncio.get_event_loop()
        t0 = loop.time()
        sent = 0
        while sent < total:
            await client.write_gatt_char(CHAR_UUID_TPUT, payload,
                                         response=False)
            sent += len(payload)
        c1 = struct.unpack("<I",
                           await client.read_gatt_char(CHAR_UUID_TPUT))[0]
        dt = loop.time() - t0

        got = (c1 - c0) & 0xFFFFFFFF
        print(f"  sent {sent} B, device counted {got} B"
              f"{'  (LOSS!)' if got != sent else ''}")
        print(f"  elapsed {dt:.2f} s -> {sent / dt / 1024:.1f} KiB/s")


async def time_sync(device):
    """Send the wall clock ('T' + u64 epoch ms) over NUS RX."""
    import time

    async with BleakClient(device,
                           winrt=dict(use_cached_services=False)) as client:
        epoch = int(time.time() * 1000)
        await client.write_gatt_char(
            NUS_RX_UUID, b"T" + struct.pack("<Q", epoch), response=False)
        print(f"Wall clock synced: {epoch} ms")


async def fetch(device, name: str, outdir: str):
    """Download a log file (or INDEX.TXT) over MCUmgr SMP FS."""
    try:
        from smpclient import SMPClient
        from smpclient.transport.ble import SMPBLETransport
    except ImportError:
        print("Fetch requires the smpclient package:  pip install smpclient",
              file=sys.stderr)
        sys.exit(1)
    import os

    os.makedirs(outdir, exist_ok=True)
    async with SMPClient(SMPBLETransport(), device.address,
                         timeout_s=20.0) as client:
        names = [name]
        if name.upper() == "INDEX":
            names = ["INDEX.TXT"]

        for n in names:
            path = f"/SD:/LOG/{n}"
            out = os.path.join(outdir, n)
            print(f"fetching {path} ...")
            data = bytearray()
            async for offset, total, chunk in _fs_download(client, path):
                data.extend(chunk)
                print(f"\r  {len(data)}/{total} B", end="", flush=True)
            with open(out, "wb") as f:
                f.write(data)
            print(f"\r  saved {out} ({len(data)} B)      ")


async def _fs_download(client, path):
    """Minimal SMP fs download generator (offset, total, chunk)."""
    from smpclient.requests.file_management import FileDownload
    from smpclient.generics import error

    offset = 0
    total = None
    while True:
        r = await client.request(FileDownload(off=offset, name=path))
        if error(r):
            raise RuntimeError(f"fs download error at offset {offset}: {r}")
        if hasattr(r, "len") and r.len is not None:
            total = r.len
        chunk = r.data
        yield offset, total or 0, chunk
        offset += len(chunk)
        if total is not None and offset >= total:
            return


async def dfu(device, path: str):
    """OTA update: upload a signed image over SMP, mark it, reboot."""
    try:
        from smpclient import SMPClient
        from smpclient.transport.ble import SMPBLETransport
        from smpclient.generics import error
        from smpclient.requests.image_management import (ImageStatesRead,
                                                         ImageStatesWrite)
        from smpclient.requests.os_management import ResetWrite
    except ImportError:
        print("DFU requires the smpclient package:  pip install smpclient",
              file=sys.stderr)
        sys.exit(1)

    with open(path, "rb") as f:
        image = f.read()
    print(f"DFU image: {path} ({len(image)} bytes)")

    print(f"Connecting (SMP) to {device.address} ...", flush=True)
    # Generous timeout: smpclient's Windows-MTU-bug workaround needs
    # several seconds of retries inside connect()
    async with SMPClient(SMPBLETransport(), device.address,
                         timeout_s=20.0) as client:
        pre = await client.request(ImageStatesRead())
        if not error(pre):
            active = next((i for i in pre.images
                           if getattr(i, "active", False)), None)
            if active is not None:
                print(f"  device runs: version {active.version}")

        loop = asyncio.get_event_loop()
        start = loop.time()
        async for offset in client.upload(image):
            pct = 100 * offset // len(image)
            print(f"\r  upload {offset}/{len(image)} B ({pct}%)",
                  end="", flush=True)
        rate = len(image) / max(loop.time() - start, 1e-9) / 1024
        print(f"\r  upload {len(image)}/{len(image)} B (100%), "
              f"{rate:.1f} KiB/s")

        states = await client.request(ImageStatesRead())
        if error(states):
            print(f"Image state read failed: {states}", file=sys.stderr)
            sys.exit(1)
        pending = next((i for i in states.images if i.slot == 1), None)
        if pending is None:
            print("Uploaded image not visible in slot 1", file=sys.stderr)
            sys.exit(1)
        print(f"  slot 1: version {pending.version}")

        marked = await client.request(
            ImageStatesWrite(hash=pending.hash, confirm=False))
        if error(marked):
            if "TEST_TO_ACTIVE_DENIED" in str(marked):
                print("Device already runs this exact image - nothing to do.")
                return
            print(f"Marking image failed: {marked}", file=sys.stderr)
            sys.exit(1)

        print("Image marked for install; rebooting device ...")
        reset = await client.request(ResetWrite())
        if error(reset):
            print(f"Reset failed: {reset}", file=sys.stderr)
            sys.exit(1)

    print("DFU sent - device installs and boots the new firmware (~10 s).")


async def main():
    parser = argparse.ArgumentParser(
        description="KMM PMask throughput test / OTA DFU")
    parser.add_argument("--tput", metavar="KIB", type=int, nargs="?",
                        const=64, default=None,
                        help="throughput test (default 64 KiB)")
    parser.add_argument("--dfu", metavar="SIGNED_BIN", default=None,
                        help="OTA update with a zephyr.signed.bin")
    parser.add_argument("--sync", action="store_true",
                        help="sync the board's wall clock (SD timestamps)")
    parser.add_argument("--fetch", metavar="FILE", default=None,
                        help="download a log file over BLE "
                             "(INDEX for the file list)")
    parser.add_argument("--outdir", default="fetched",
                        help="output directory for --fetch")
    parser.add_argument("--name", default=DEVICE_PREFIX,
                        help=f"device name prefix (default {DEVICE_PREFIX})")
    args = parser.parse_args()

    if (args.tput is None and args.dfu is None and not args.sync
            and args.fetch is None):
        parser.error("pick a function: --tput [KiB], --dfu <image>, "
                     "--sync or --fetch <file|INDEX>")

    device = await discover(args.name)

    if args.sync:
        await time_sync(device)
    if args.fetch is not None:
        await fetch(device, args.fetch, args.outdir)
    if args.dfu is not None:
        await dfu(device, args.dfu)
    if args.tput is not None:
        await tput_test(device, args.tput)


if __name__ == "__main__":
    asyncio.run(main())
