#!/usr/bin/env python3
"""vdb: host side of the Vita debug bridge.

  vdb.py logs                      tail the Vita's log ring over USB (interface 0)
  vdb.py ping                      round trip over the command pipe
  vdb.py status                    ring/uptime status
  vdb.py ls  ux0:data              list a directory (any device: ux0:, ur0:, uma0:, vs0:, os0: ...)
  vdb.py stat ux0:data/x.bin       one entry
  vdb.py get ux0:data/x.bin [dst]  copy a file from the Vita (dst defaults to the basename)
  vdb.py put src ux0:data/x.bin    copy a file to the Vita (created or truncated)
  vdb.py mkdir ux0:data/dir
  vdb.py rm ux0:data/x.bin
  vdb.py rmdir ux0:data/dir
  vdb.py mv ux0:data/a ux0:data/b
  vdb.py reboot                    cold reboot (also: reboot soft, poweroff, suspend)

`logs` and the other commands use different USB interfaces, so a log tail can
stay attached in one terminal while files move in another.

Needs pyusb (pip install pyusb) and libusb (brew install libusb).
"""
import os
import struct
import sys
import time

import usb.core
import usb.util

VID = 0x054C   # Sony, filled in by the Vita's USB stack
PID = 0x0DB6   # vdb
EP_IN = 0x81   # interface 0: log ring
EP_OUT = 0x02  # interface 1: commands
EP_DATA = 0x83 # interface 1: replies and file data

CMD_MAGIC = 0x31424456  # "VDB1"
RSP_MAGIC = 0x52424456  # "VDBR"
HDR = struct.Struct("<IIII")   # cmd: magic, cmd, len, arg; rsp: magic, cmd, status(i), len
RSP = struct.Struct("<IIiI")

(CMD_PING, CMD_STATUS, CMD_LIST, CMD_STAT, CMD_GET, CMD_PUT_OPEN, CMD_PUT_DATA,
 CMD_PUT_CLOSE, CMD_MKDIR, CMD_REMOVE, CMD_RMDIR, CMD_RENAME, CMD_POWER) = range(1, 14)
POWER_ACTIONS = {"reboot": 0, "soft": 1, "poweroff": 2, "suspend": 3}

RX_SIZE_OLD = 4096             # the command buffer of vdb 0.2 (no "rx" in its status line)
DATA_CHUNK = 32 * 1024
TIMEOUT = 5000


def open_device(interface):
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit("no vdb device (Vita not connected, plugin idle, or usb_on flag missing)")
    try:
        dev.get_active_configuration()
    except usb.core.USBError:
        try:
            dev.set_configuration()
        except usb.core.USBError as e:
            print(f"set_configuration: {e}", file=sys.stderr)
    usb.util.claim_interface(dev, interface)
    return dev


class Bridge:
    """Command side: interface 1, EP2 out, EP3 in."""

    def __init__(self):
        self.dev = open_device(1)
        cfg = self.dev.get_active_configuration()
        ep = usb.util.find_descriptor(cfg[(1, 0)], bEndpointAddress=EP_OUT)
        self.packet = ep.wMaxPacketSize if ep is not None else 512
        # the plugin's command buffer sizes our frames: ask, since a frame
        # longer than the buffer is silently cut (a 0.2 plugin fed 64 KB
        # frames wrote 4 KB of each)
        self.rx_size = RX_SIZE_OLD
        self.send(CMD_STATUS)
        _, status, data = self.reply()
        text = data.decode("utf-8", "replace")
        if status == 0 and ", rx " in text:
            self.rx_size = int(text.rsplit(", rx ", 1)[1].split()[0])
        self.status_text = text
        self.put_chunk = self.rx_size - HDR.size

    def send(self, cmd, payload=b"", arg=0):
        frame = HDR.pack(CMD_MAGIC, cmd, len(payload), arg) + payload
        if len(frame) > self.rx_size:
            raise ValueError("frame too long")
        # a transfer that is an exact multiple of the packet size has no short
        # packet to end it; the plugin ignores the pad byte (len is authoritative)
        if len(frame) < self.rx_size and len(frame) % self.packet == 0:
            frame += b"\0"
        self.dev.write(EP_OUT, frame, timeout=TIMEOUT)

    def reply(self, timeout=TIMEOUT):
        data = bytes(self.dev.read(EP_DATA, RSP.size + DATA_CHUNK, timeout=timeout))
        if len(data) < RSP.size:
            raise IOError(f"short reply ({len(data)} bytes)")
        magic, cmd, status, length = RSP.unpack_from(data)
        if magic != RSP_MAGIC:
            raise IOError(f"bad reply magic 0x{magic:08x}")
        return cmd, status, data[RSP.size:RSP.size + length]

    def simple(self, cmd, payload=b""):
        self.send(cmd, payload)
        _, status, data = self.reply()
        return status, data


def path_arg(p):
    return p.encode("utf-8") + b"\0"


def check(status, what):
    if status < 0:
        sys.exit(f"{what}: error 0x{status & 0xFFFFFFFF:08x}")


def logs():
    dev = open_device(0)
    print("vdb: attached, waiting for log data (ctrl-c to stop)", file=sys.stderr)
    buf = b""
    while True:
        try:
            data = dev.read(EP_IN, 4096, timeout=2000)
        except usb.core.USBTimeoutError:
            continue
        except usb.core.USBError as e:
            print(f"usb error: {e}", file=sys.stderr)
            time.sleep(0.5)
            continue
        buf += bytes(data)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            print(line.decode("utf-8", "replace"), flush=True)


def cmd_ls(b, path):
    b.send(CMD_LIST, path_arg(path))
    while True:
        _, status, data = b.reply()
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            continue
        sys.stdout.flush()
        check(status, f"ls {path}")
        print(f"{status} entries", file=sys.stderr)
        return


def cmd_get(b, src, dst=None):
    dst = dst or os.path.basename(src.split(":", 1)[-1].rstrip("/")) or "vdb.out"
    b.send(CMD_GET, path_arg(src))
    _, status, data = b.reply()
    check(status, f"get {src}")
    (size,) = struct.unpack("<Q", data[:8])
    t0 = time.time()
    done = 0
    with open(dst, "wb") as f:
        while True:
            _, status, data = b.reply(timeout=15000)
            if not data:
                check(status, f"get {src}")
                break
            f.write(data)
            done += len(data)
            if done % (1 << 20) < DATA_CHUNK:
                print(f"\r{done}/{size} bytes", end="", file=sys.stderr, flush=True)
    dt = max(time.time() - t0, 1e-6)
    print(f"\r{src} -> {dst}: {done} bytes in {dt:.1f} s ({done / dt / 1e6:.1f} MB/s)   ", file=sys.stderr)
    if done != size:
        sys.exit(f"size mismatch: expected {size}")


def cmd_put(b, src, dst):
    size = os.path.getsize(src)
    status, _ = b.simple(CMD_PUT_OPEN, path_arg(dst))
    check(status, f"put open {dst}")
    t0 = time.time()
    done = 0
    with open(src, "rb") as f:
        while True:
            chunk = f.read(b.put_chunk)
            if not chunk:
                break
            b.send(CMD_PUT_DATA, chunk)
            done += len(chunk)
            if done % (1 << 20) < b.put_chunk:
                print(f"\r{done}/{size} bytes", end="", file=sys.stderr, flush=True)
    status, data = b.simple(CMD_PUT_CLOSE)
    (written,) = struct.unpack("<Q", data[:8]) if len(data) >= 8 else (0,)
    dt = max(time.time() - t0, 1e-6)
    print(f"\r{src} -> {dst}: {written} bytes in {dt:.1f} s ({written / dt / 1e6:.1f} MB/s)   ", file=sys.stderr)
    check(status, f"put {dst}")
    if written != size:
        sys.exit(f"size mismatch: wrote {written}, expected {size}")


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    cmd, rest = args[0], args[1:]
    if cmd == "logs":
        logs()
        return 0
    b = Bridge()
    if cmd == "ping":
        t0 = time.time()
        status, _ = b.simple(CMD_PING)
        check(status, "ping")
        print(f"pong in {(time.time() - t0) * 1000:.1f} ms")
    elif cmd == "status":
        sys.stdout.write(b.status_text)
    elif cmd == "ls" and len(rest) == 1:
        cmd_ls(b, rest[0])
    elif cmd == "stat" and len(rest) == 1:
        status, data = b.simple(CMD_STAT, path_arg(rest[0]))
        check(status, f"stat {rest[0]}")
        sys.stdout.write(data.decode("utf-8", "replace"))
    elif cmd == "get" and len(rest) in (1, 2):
        cmd_get(b, *rest)
    elif cmd == "put" and len(rest) == 2:
        cmd_put(b, rest[0], rest[1])
    elif cmd == "mkdir" and len(rest) == 1:
        check(b.simple(CMD_MKDIR, path_arg(rest[0]))[0], f"mkdir {rest[0]}")
    elif cmd == "rm" and len(rest) == 1:
        check(b.simple(CMD_REMOVE, path_arg(rest[0]))[0], f"rm {rest[0]}")
    elif cmd == "rmdir" and len(rest) == 1:
        check(b.simple(CMD_RMDIR, path_arg(rest[0]))[0], f"rmdir {rest[0]}")
    elif cmd == "mv" and len(rest) == 2:
        check(b.simple(CMD_RENAME, path_arg(rest[0]) + path_arg(rest[1]))[0], f"mv {rest[0]}")
    elif cmd in ("reboot", "poweroff", "suspend") and len(rest) <= 1:
        action = "soft" if (cmd == "reboot" and rest == ["soft"]) else cmd
        if action not in POWER_ACTIONS:
            print(__doc__)
            return 1
        b.send(CMD_POWER, arg=POWER_ACTIONS[action])
        _, status, _ = b.reply()
        check(status, action)
        print(f"{action} requested; the Vita leaves the bus in a moment")
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
