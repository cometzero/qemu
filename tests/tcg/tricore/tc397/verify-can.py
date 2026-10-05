#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""TC397 M_CAN active MMIO/IRQ tests with an explicit mock transport peer.

Mock acknowledgements verify hardware semantics, not SIL Kit delivery.
"""
import argparse
import hashlib
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time
import zlib

BASE = 0xf0208000
RAM = 0xf0200000
CORE = BASE + 0x200
SRC = 0xf0038000 + 0x16c * 4


def wire(kind, epoch, token=0, flags=0, dlc=0, ident=0, status=0, data=b""):
    body = struct.pack("<4sBBBBIIII64s", b"CAN1", kind, flags, dlc,
                       len(data), epoch, token, ident, status, data)
    return body + struct.pack("<I", zlib.crc32(body))


def connect(path):
    deadline = time.monotonic() + 5
    while True:
        sock = socket.socket(socket.AF_UNIX)
        sock.settimeout(2)
        try:
            sock.connect(str(path))
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close()
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)


def exercise(args, result):
    checks, transcript = result["checks"], []
    with tempfile.TemporaryDirectory(prefix="tc397-can-") as tmp:
        command = [str(args.qemu.resolve()), "-M", "KIT_AURIX_TC397B_TRB",
                   "-display", "none", "-monitor", "none", "-serial", "null",
                   "-S", "-qtest", "stdio", "-qmp",
                   f"unix:{tmp}/qmp,server=on,wait=off", "-chardev",
                   f"socket,id=can,path={tmp}/can,server=on,wait=off",
                   "-global", "tc397-can.chardev=can", "-global",
                   "tc397-can.ack-timeout-ms=500"]
        result["command"] = command
        with (args.output / "qemu.log").open("w") as log:
            child = subprocess.Popen(command, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=log,
                                     text=True, bufsize=1)
            peers = []
            try:
                peer = connect(Path(tmp) / "can")
                peers.append(peer)
                qmp = connect(Path(tmp) / "qmp")
                peers.append(qmp)
                qmp_stream = qmp.makefile("rwb")
                assert "QMP" in json.loads(qmp_stream.readline())

                def qmp_cmd(name):
                    qmp_stream.write(json.dumps({"execute": name}).encode() + b"\n")
                    qmp_stream.flush()
                    while True:
                        reply = json.loads(qmp_stream.readline())
                        assert "error" not in reply, reply
                        if "return" in reply:
                            break

                qmp_cmd("qmp_capabilities")

                def qt(command):
                    child.stdin.write(command + "\n")
                    child.stdin.flush()
                    assert select.select([child.stdout], [], [], 3)[0], command
                    reply = child.stdout.readline().strip()
                    transcript.append([command, reply])
                    assert reply.startswith("OK"), reply
                    return reply

                def write(address, value):
                    qt(f"writel {address:#x} {value:#x}")

                def read(address):
                    return int(qt(f"readl {address:#x}").split()[1], 16)

                def receive(kind=None, status=None):
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline:
                        data = bytearray()
                        while len(data) < 92:
                            part = peer.recv(92 - len(data))
                            assert part, "CAN EOF"
                            data.extend(part)
                        assert data[:4] == b"CAN1" and zlib.crc32(data[:88]) == \
                            struct.unpack_from("<I", data, 88)[0]
                        values = struct.unpack("<4sBBBBIIII64sI", data)
                        frame = dict(zip(["magic", "kind", "flags", "dlc", "length",
                                          "epoch", "token", "id", "status", "data",
                                          "crc"], values))
                        transcript.append(["wire RX", bytes(data).hex()])
                        if (kind is None or frame["kind"] == kind) and \
                                (status is None or frame["status"] == status):
                            return frame
                    raise TimeoutError("CAN peer frame")

                def send(frame):
                    transcript.append(["wire TX", frame.hex()])
                    peer.sendall(frame)
                    time.sleep(0.01)

                def mark(name, condition=True):
                    assert condition, name
                    checks[name] = "PASS"

                def initialize():
                    write(BASE, 0)
                    write(CORE + 0x18, 3)
                    write(CORE + 0x18, 0x303)
                    write(BASE + 0x108, 0)
                    write(BASE + 0x10c, 0x7ffc)
                    write(BASE + 0x114, 0)
                    write(BASE + 0x118, 0x11000)
                    write(CORE + 0x84, 2 << 16)
                    write(CORE + 0x88, (2 << 16) | 0x20)
                    write(CORE + 0x80, 0x28)
                    write(CORE + 0xa0, (2 << 16) | 0x100)
                    write(CORE + 0xb0, (2 << 16) | 0x200)
                    write(CORE + 0xbc, 0x777)
                    write(CORE + 0xc0, (4 << 24) | 0x400 | (1 << 30))
                    write(CORE + 0xc8, 7)
                    write(CORE + 0xf0, (2 << 16) | 0x300)
                    write(CORE + 0xe0, 0xffffffff)
                    write(CORE + 0x54, 0x1fffffff)
                    write(RAM, (2 << 30) | (1 << 27) | (0x123 << 16) | 0x7ff)
                    write(RAM + 4, (2 << 30) | (2 << 27) | (0x456 << 16) | 0x7ff)
                    write(RAM + 0x20, (1 << 29) | 0x1abcde)
                    write(RAM + 0x24, (2 << 30) | 0x1fffffff)
                    write(SRC, 13 | (1 << 10) | (1 << 25))
                    write(SRC + 4, 14 | (1 << 10) | (1 << 25))
                    write(CORE + 0x18, 0x300)
                    return receive(4, 1)["epoch"]

                def submit(index, ident, data, flags=0, dlc=None):
                    if dlc is None:
                        dlc = len(data)
                    h0 = ident | (1 << 30) if flags & 1 else ident << 18
                    h0 |= (1 << 29) if flags & 2 else 0
                    h1 = dlc << 16 | index << 24 | (1 << 23)
                    h1 |= (1 << 21) if flags & 4 else 0
                    h1 |= (1 << 20) if flags & 8 else 0
                    offset = RAM + 0x400 + index * 72
                    write(offset, h0)
                    write(offset + 4, h1)
                    for pos in range(0, len(data), 4):
                        write(offset + 8 + pos,
                              int.from_bytes(data[pos:pos + 4].ljust(4, b"\0"), "little"))
                    write(CORE + 0xd0, 1 << index)
                    return receive(1)

                def ack(frame, status=1, epoch=None, token=None):
                    send(wire(3, frame["epoch"] if epoch is None else epoch,
                              frame["token"] if token is None else token, status=status))

                def clear_event():
                    status = read(CORE + 0xf4)
                    write(CORE + 0xf8, (status >> 8) & 31)
                    write(CORE + 0x50, 0xffffffff)
                    write(SRC, 13 | (1 << 10) | (1 << 25))

                receive(4, 2)
                mark("reset-and-endian", read(CORE + 0x18) == 1 and
                     read(CORE + 4) == 0x87654321)
                epoch = initialize()
                mark("init-start-control", epoch != 0 and read(CORE + 0x18) == 0x300)
                frame = submit(0, 0x123, b"CANtest!")
                mark("classic-tx-wire", frame["id"] == 0x123 and frame["dlc"] == 8 and
                     frame["data"][:8] == b"CANtest!" and frame["epoch"] == epoch)
                mark("no-success-before-ack", read(CORE + 0xcc) == 1 and
                     read(CORE + 0xd8) == 0 and read(CORE + 0xf4) == 0)
                ack(frame, epoch=epoch - 1)
                ack(frame, token=frame["token"] + 100)
                mark("stale-ack-ignored", read(CORE + 0xcc) == 1 and read(CORE + 0xf4) == 0)
                ack(frame)
                mark("ack-tx-event-and-src364", read(CORE + 0xcc) == 0 and
                     read(CORE + 0xd8) == 1 and read(CORE + 0xf4) & 0x3f == 1 and
                     bool(read(SRC) & (1 << 24)))
                mark("tx-event-marker", read(RAM + 0x304) >> 24 == 0 and
                     bool(read(RAM + 0x304) & (1 << 22)))
                clear_event()
                ack(frame)
                mark("duplicate-ack-idempotent", read(CORE + 0xf4) & 0x3f == 0)

                data64 = bytes(range(64))
                fd = submit(1, 0x1abcde, data64, flags=13, dlc=15)
                mark("extended-fd64-brs-tx", fd["flags"] == 13 and fd["dlc"] == 15 and
                     fd["length"] == 64 and fd["data"] == data64)
                ack(fd)
                mark("fd-tx-event-marker", read(RAM + 0x30c) >> 24 == 1)
                clear_event()

                send(wire(2, epoch, ident=0x124, dlc=1, data=b"X"))
                mark("standard-filter-reject", read(CORE + 0xa4) & 0x7f == 0)
                send(wire(2, epoch, ident=0x123, dlc=4, data=b"RXok"))
                mark("rx-fifo0-and-src365", read(CORE + 0xa4) & 0x7f == 1 and
                     read(RAM + 0x108) == int.from_bytes(b"RXok", "little") and
                     bool(read(SRC + 4) & (1 << 24)))
                write(CORE + 0xa8, 0)
                mark("rx-ack-consumes", read(CORE + 0xa4) & 0x7f == 0)
                send(wire(2, epoch, ident=0x456, dlc=1, data=b"1"))
                mark("standard-filter-fifo1", read(CORE + 0xb4) & 0x7f == 1)
                write(CORE + 0xb8, 0)
                send(wire(2, epoch, flags=29, ident=0x1abcde, dlc=15, data=data64))
                mark("extended-fd-rx-header", read(CORE + 0xa4) & 0x7f == 1 and
                     read(RAM + 0x148) & (1 << 30) and
                     read(RAM + 0x14c) & (1 << 21) and
                     read(RAM + 0x150) == 0x03020100)
                write(CORE + 0xa8, 1)
                send(wire(2, epoch, flags=2, ident=0x123, dlc=8))
                mark("rtr-rx", bool(read(RAM + 0x100) & (1 << 29)))
                write(CORE + 0xa8, 0)

                malformed = bytearray(wire(2, epoch, ident=0x123, dlc=1, data=b"N"))
                malformed[24] ^= 1
                send(bytes(malformed))
                send(wire(2, epoch, flags=12, ident=0x123, dlc=15, data=b"short"))
                send(wire(2, epoch, flags=6, ident=0x123, dlc=0))
                send(wire(2, epoch - 1, ident=0x123, dlc=0))
                mark("bad-crc-flags-length-epoch-rejected", read(CORE + 0xa4) & 0x7f == 0)
                incoming = wire(2, epoch, ident=0x123, dlc=2, data=b"OK")
                send(b"noiseCAN0" + incoming[:37])
                mark("fragment-not-visible", read(CORE + 0xa4) & 0x7f == 0)
                send(incoming[37:])
                mark("fragment-resync", read(CORE + 0xa4) & 0x7f == 1)
                send(incoming)
                send(incoming)
                mark("rx-fifo-bounded-overflow", read(CORE + 0xa4) & 0x7f == 2 and
                     bool(read(CORE + 0x50) & (1 << 3)))

                pending = submit(2, 0x123, b"reset")
                qmp_cmd("system_reset")
                stopped = receive(4, 2)
                mark("reset-cancels-session", stopped["epoch"] != epoch and
                     read(CORE + 0xcc) == 0 and read(CORE + 0xa4) == 0)
                epoch = initialize()
                ack(pending)
                mark("reset-old-ack-ignored", read(CORE + 0xf4) == 0)
                failed = submit(0, 0x123, b"fail")
                ack(failed, status=3)
                mark("failed-ack-busoff-no-success", read(CORE + 0x44) & (1 << 7) and
                     read(CORE + 0x50) & (1 << 25) and read(CORE + 0xf4) == 0)

                qmp_cmd("system_reset")
                receive(4, 2)
                epoch = initialize()
                noack = submit(0, 0x123, b"timeout")
                time.sleep(0.6)
                mark("missing-ack-timeout", read(CORE + 0x44) & (1 << 7) and
                     read(CORE + 0xf4) == 0 and read(CORE + 0xcc) == 0)
                ack(noack)
                mark("late-timeout-ack-ignored", read(CORE + 0xf4) == 0)
                qmp_cmd("system_reset")
                receive(4, 2)
                epoch = initialize()
                submit(0, 0x123, b"drop")
                peer.close()
                time.sleep(0.05)
                mark("disconnect-busoff", read(CORE + 0x44) & (1 << 7) and
                     read(CORE + 0xcc) == 0)
                peer = connect(Path(tmp) / "can")
                peers.append(peer)
                mark("reconnect-is-stopped", receive(4, 2)["epoch"] != epoch)
                qmp_stream.close()
            finally:
                for peer in peers:
                    peer.close()
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
                (args.output / "traffic.json").write_text(
                    json.dumps(transcript, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"status": "FAIL", "checks": {}, "method": "qtest mock CAN peer",
              "qemu_sha256": hashlib.sha256(args.qemu.read_bytes()).hexdigest()}
    try:
        exercise(args, result)
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = str(error)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
