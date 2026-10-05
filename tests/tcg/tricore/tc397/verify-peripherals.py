#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Active qtest traffic for TC397 UARTs and the independent PORT0 pin link.

This verifies device semantics while the CPU is paused; it is not guest boot.
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


def connect(path):
    deadline = time.monotonic() + 5
    while True:
        stream = socket.socket(socket.AF_UNIX)
        stream.settimeout(2)
        try:
            stream.connect(str(path))
            return stream
        except (FileNotFoundError, ConnectionRefusedError):
            stream.close()
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)


def run(qemu, output, result):
    transcript = []
    checks = result["checks"]
    with tempfile.TemporaryDirectory(prefix="tc397-pin-") as tmp:
        directory = Path(tmp)
        command = [str(qemu), "-M", "KIT_AURIX_TC397B_TRB", "-display", "none",
                   "-monitor", "none", "-S", "-qtest", "stdio", "-qmp",
                   f"unix:{directory}/qmp,server=on,wait=off"]
        for index in range(3):
            command += ["-serial", f"unix:{directory}/u{index},server=on,wait=off"]
        command += ["-chardev", f"socket,id=tc397gpio,path={directory}/gpio,"
                    "server=on,wait=off", "-global",
                    "tricore-port.chardev=tc397gpio"]
        result["command"] = command
        with (output / "qemu.log").open("w") as log:
            child = subprocess.Popen(command, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=log,
                                     text=True, bufsize=1)
            streams = []
            try:
                def qt(request):
                    child.stdin.write(request + "\n")
                    child.stdin.flush()
                    if not select.select([child.stdout], [], [], 3)[0]:
                        raise TimeoutError(request)
                    response = child.stdout.readline().strip()
                    transcript.append([request, response])
                    assert response.startswith("OK"), response
                    return response

                def read(address):
                    return int(qt(f"readl {address:#x}").split()[1], 16)

                def write(address, value):
                    qt(f"writel {address:#x} {value:#x}")

                def mark(name, condition=True):
                    assert condition, name
                    checks[name] = "PASS"

                uarts = [connect(directory / f"u{i}") for i in range(3)]
                streams.extend(uarts)
                gpio = connect(directory / "gpio")
                streams.append(gpio)
                qmp = connect(directory / "qmp")
                streams.append(qmp)
                qmp_file = qmp.makefile("rwb")
                assert "QMP" in json.loads(qmp_file.readline())

                def qmp_command(name):
                    qmp_file.write(json.dumps({"execute": name}).encode() + b"\n")
                    qmp_file.flush()
                    while True:
                        message = json.loads(qmp_file.readline())
                        if "return" in message:
                            return
                        assert "error" not in message, message

                qmp_command("qmp_capabilities")

                def receive_frame(levels=None, mask=None):
                    deadline = time.monotonic() + 2
                    while time.monotonic() < deadline:
                        raw = bytearray()
                        while len(raw) < 8:
                            chunk = gpio.recv(8 - len(raw))
                            assert chunk, "GPIO EOF"
                            raw.extend(chunk)
                        assert raw[:4] == b"GP\x01O", raw
                        actual = struct.unpack("<HH", raw[4:])
                        transcript.append(["GPIO RX", raw.hex()])
                        if levels is None or actual == (levels, mask):
                            return actual
                    raise TimeoutError(f"GPIO levels={levels} mask={mask}")

                def send_input(levels, mask):
                    gpio.sendall(b"GP\x01I" + struct.pack("<HH", levels, mask))
                    time.sleep(0.02)

                mark("gpio-connect-release-snapshot", receive_frame() == (0, 0))
                for index, uart in enumerate(uarts):
                    base = 0xf0000600 + index * 0x100
                    source = 0xf0038000 + (0x15 + index * 3) * 4
                    write(base, 0)
                    write(base + 0x10, 2)
                    write(base + 0x40, 1 << 28)
                    uart.sendall(bytes([0x41 + index]))
                    time.sleep(0.02)
                    mark(f"uart{index}-rx-src",
                         (read(base + 0x10) >> 16) == 1 and
                         bool(read(source) & (1 << 24)))
                    mark(f"uart{index}-rx-data", read(base + 0x48) == 0x41 + index)
                    write(base + 0x44, 0x51 + index)
                    mark(f"uart{index}-tx-data", uart.recv(1) == bytes([0x51 + index]))
                mark("uart-fifo-isolation", all(
                    read(0xf0000610 + index * 0x100) >> 16 == 0
                    for index in range(3)))

                base = 0xf003a000
                write(base, 8)
                mark("output-latch-without-drive", read(base) == 8 and
                     read(base + 0x24) == 0 and receive_frame(0, 0) == (0, 0))
                write(base + 0x10, 0x80100000)  # pin2 pull-up, pin3 output
                write(base + 0x14, 0x80)  # pin4 output
                mark("gpio-output-enable", receive_frame(8, 0x18) == (8, 0x18))
                mark("gpio-pullup-output-readback", read(base + 0x24) == 12)
                send_input(1, 7)
                mark("gpio-external-input", read(base + 0x24) == 9)
                write(base + 0x24, 0xffff)
                mark("gpio-input-readonly", read(base + 0x24) == 9)

                write(base + 4, 8 << 16)
                mark("gpio-omr-clear", read(base) == 0 and
                     receive_frame(0, 24) == (0, 24))
                write(base + 4, 8)
                write(base + 4, 8)  # Repeated set must remain high.
                mark("gpio-omr-set-idempotent", read(base) == 8)
                write(base + 4, (8 << 16) | 8)
                mark("gpio-omr-toggle-low", read(base) == 0)
                write(base + 4, (8 << 16) | 8)
                mark("gpio-omr-toggle-high", read(base) == 8 and read(base + 4) == 0)

                frame = b"GP\x01I" + struct.pack("<HH", 6, 7)
                gpio.sendall(b"invalid GP\x02I" + b"X" * 1024 + frame[:3])
                time.sleep(0.02)
                mark("gpio-partial-frame-no-change", read(base + 0x24) == 9)
                gpio.sendall(frame[3:])
                time.sleep(0.02)
                mark("gpio-frame-resync", read(base + 0x24) == 14)

                write(base + 0x14, 0xc0)
                write(base + 4, 16)
                mark("gpio-open-drain-release", receive_frame(8, 8) == (8, 8))
                write(base + 4, 16 << 16)
                mark("gpio-open-drain-low", receive_frame(8, 24) == (8, 24))
                write(base + 0x14, 0x88)
                mark("gpio-alternate-output-undriven", receive_frame(8, 8) == (8, 8))

                # Pause is explicit (-S); periodic host-time snapshots still arrive.
                start = time.monotonic()
                while time.monotonic() - start < 0.25:
                    receive_frame(8, 8)
                mark("gpio-host-snapshot-while-cpu-paused")
                # Deliberately stop reading while register writes produce edges.
                # The device must not block qtest, and must retain frame alignment.
                for index in range(4096):
                    write(base, (index & 1) << 3)
                write(base + 0x14, 0x80)
                write(base, 16)
                mark("gpio-backpressure-latest-snapshot",
                     receive_frame(16, 24) == (16, 24))
                write(base + 0x14, 0x88)
                write(base, 8)
                receive_frame(8, 8)
                gpio.close()
                time.sleep(0.05)
                mark("gpio-disconnect-releases-input", read(base + 0x24) == 12)
                gpio = connect(directory / "gpio")
                streams.append(gpio)
                mark("gpio-reconnect-current-snapshot", receive_frame() == (8, 8))
                send_input(2, 7)
                qmp_command("system_reset")
                mark("gpio-reset-registers", read(base) == 0 and
                     all(read(base + 0x10 + i * 4) == 0 for i in range(4)))
                mark("gpio-reset-releases-output", receive_frame(0, 0) == (0, 0))
                mark("gpio-reset-retains-external-levels", read(base + 0x24) == 2)
                mark("uart-reset-clears-fifos", all(
                    read(0xf0000610 + i * 0x100) >> 16 == 0 for i in range(3)))
                qmp_file.close()
            finally:
                for stream in streams:
                    stream.close()
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
                (output / "traffic.json").write_text(
                    json.dumps(transcript, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {"status": "FAIL", "method": "qtest, CPU paused; not guest boot",
              "checks": {},
              "qemu_sha256": hashlib.sha256(args.qemu.read_bytes()).hexdigest()}
    try:
        run(args.qemu.resolve(), args.output, result)
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = str(error)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
