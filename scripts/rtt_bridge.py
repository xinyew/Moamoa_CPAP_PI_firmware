#!/usr/bin/env python3
"""
RTT -> WebSocket bridge for the CPAP PI web portal.

Reads the firmware's binary sensor stream from SEGGER RTT up-buffer 1
("data") over the debug probe, re-frames it on the 0xC9A5 magic, and
broadcasts complete frames to WebSocket clients (the portal's
"RTT (wired)" mode connects to ws://localhost:8765).

Requires: pip install pylink-square websockets
Note: close any other RTT session (commander-cli / RTT Viewer) first —
the probe supports one debug connection at a time.

Usage:
    python rtt_bridge.py [--port 8765] [--device NRF52840_XXAA]
"""

import argparse
import asyncio
import collections
import struct
import sys
import threading

import pylink
import websockets

# There are TWO packages on PyPI that install as `import pylink`: the SEGGER
# J-Link binding we need is `pylink-square` (provides pylink.JLink); the
# unrelated `PyLink` "File-Like API" package shadows the name and has no JLink.
# If the wrong one is installed the bridge dies with a cryptic AttributeError,
# so fail loudly with the fix instead.
if not hasattr(pylink, "JLink"):
    sys.exit(
        "ERROR: the installed 'pylink' is the wrong package (no pylink.JLink).\n"
        "       Fix with:  pip uninstall -y pylink && pip install pylink-square\n"
        f"       (using: {getattr(pylink, '__file__', '?')})"
    )

MAGIC = 0xC9A5
FRAME_LEN = {0x11: 204, 0x12: 45}  # v2 type -> length (comm_protocol.h)
RTT_CHANNEL = 1

clients = set()
stats = {"data": 0, "status": 0, "bytes": 0}


class Reframer:
    """Byte stream -> complete frames, resyncing on the magic."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, chunk: bytes):
        self.buf.extend(chunk)
        frames = []
        while True:
            # hunt for magic
            while len(self.buf) >= 2 and struct.unpack_from("<H", self.buf, 0)[0] != MAGIC:
                self.buf.pop(0)
            if len(self.buf) < 3:
                break
            length = FRAME_LEN.get(self.buf[2])
            if length is None:
                self.buf.pop(0)  # false magic — resync
                continue
            if len(self.buf) < length:
                break
            frames.append(bytes(self.buf[:length]))
            del self.buf[:length]
        return frames


def rtt_reader_thread(jlink, frame_q):
    """Blocking tight loop in a dedicated thread — RTT throughput on the
    EDU Mini scales with rtt_read call frequency (~5 B/call), so the
    asyncio loop must never pace the reads.
    """
    reframer = Reframer()
    while True:
        # Drain the console channel too — a full DLL host buffer for
        # ch0 throttles the whole RTT polling path.
        jlink.rtt_read(0, 4096)
        data = jlink.rtt_read(RTT_CHANNEL, 16384)
        if data:
            stats["bytes"] += len(data)
            for frame in reframer.feed(bytes(data)):
                stats["data" if frame[2] == 0x11 else "status"] += 1
                frame_q.append(frame)


async def frame_broadcaster(frame_q):
    while True:
        while frame_q:
            frame = frame_q.popleft()
            if clients:
                websockets.broadcast(clients, frame)
        await asyncio.sleep(0.005)


async def stats_printer():
    while True:
        await asyncio.sleep(5)
        print(f"[bridge] {stats['data']} data / {stats['status']} status frames, "
              f"{stats['bytes'] / 1024:.1f} kB read, {len(clients)} client(s)",
              flush=True)


async def handler(websocket):
    clients.add(websocket)
    print(f"[bridge] portal connected ({len(clients)} client(s))", flush=True)
    try:
        await websocket.wait_closed()
    finally:
        clients.discard(websocket)
        print(f"[bridge] portal disconnected", flush=True)


async def main():
    parser = argparse.ArgumentParser(description="CPAP PI RTT->WebSocket bridge")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--device", default="NRF52840_XXAA")
    parser.add_argument("--serial", type=int, default=None,
                        help="J-Link probe serial number (optional)")
    args = parser.parse_args()

    jlink = pylink.JLink()
    jlink.open(serial_no=args.serial)
    jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
    jlink.connect(args.device, speed=4000)
    jlink.rtt_start()
    print(f"[bridge] RTT attached to {args.device}", flush=True)

    # Wait for the RTT control block, then confirm up-buffer 1 is the firmware's
    # "data" channel. rtt_get_num_up_buffers() returns the compile-time MAX (3),
    # not how many are configured, so it can't tell us the channel is live —
    # read the buffer descriptor's name instead.
    named = False
    for _ in range(50):
        try:
            desc = jlink.rtt_get_buf_descriptor(RTT_CHANNEL, up=True)
            if desc.name:
                print(f"[bridge] up-buffer {RTT_CHANNEL} = "
                      f"'{desc.name}' ({desc.SizeOfBuffer} B)", flush=True)
                named = True
                break
        except (pylink.errors.JLinkRTTException, AttributeError):
            pass
        await asyncio.sleep(0.1)
    if not named:
        print(f"[bridge] WARNING: up-buffer {RTT_CHANNEL} not found/named. "
              "Is the firmware the RTT-streaming build (main branch), running, "
              "and past sensor bring-up? Streaming anyway; watch 'kB read'.",
              flush=True)

    frame_q = collections.deque(maxlen=512)
    threading.Thread(target=rtt_reader_thread, args=(jlink, frame_q),
                     daemon=True).start()

    async with websockets.serve(handler, "localhost", args.port):
        print(f"[bridge] serving ws://localhost:{args.port}", flush=True)
        await asyncio.gather(frame_broadcaster(frame_q), stats_printer())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
