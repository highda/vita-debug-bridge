#!/usr/bin/env python3
"""vdb: host side of the Vita debug bridge.

  vdb.py logs [--ts] [--file f]    tail the Vita's log ring over USB (interface 0); --ts stamps
                                   each line with the Mac's time, --file also appends to a file
  vdb.py run ABCD12345 [game.vpk]  install (when a VPK is given), launch, then tail the logs
  vdb.py ping                      round trip over the command pipe
  vdb.py status                    ring/uptime status
  vdb.py ls  ux0:data              list a directory (any device: ux0:, ur0:, uma0:, vs0:, os0: ...)
  vdb.py stat ux0:data/x.bin       one entry
  vdb.py get ux0:data/x.bin [dst]  copy a file from the Vita (dst defaults to the basename)
  vdb.py put src ux0:data/x.bin    copy a file to the Vita (created or truncated)
  vdb.py install game.vpk ABCD12345  write a VPK's files over an installed app (ux0:app/<id>/),
                                   no VitaShell; the title must already be installed once
  vdb.py mkdir ux0:data/dir
  vdb.py rm ux0:data/x.bin
  vdb.py rmdir ux0:data/dir
  vdb.py mv ux0:data/a ux0:data/b
  vdb.py reboot                    cold reboot (also: reboot soft, poweroff, suspend)
  vdb.py launch ABCD12345          start an installed app through the shell (needs vdb_shell.suprx)
  vdb.py kill [pid]                kill a process (default: the one that owns the screen; never the shell)
  vdb.py screen [out.png]          grab the screen (default: screen-<time>.png)
  vdb.py touch X Y [--hold 100]    a finger on the front panel (screen pixels, 960x544)
  vdb.py swipe X0 Y0 X1 Y1 [--ms 400]
  vdb.py unlock                    peel the lock screen (a swipe from the top right corner)
  vdb.py input cross [--hold 200]  press buttons and set sticks for a while:
                                   buttons: up down left right cross circle square triangle
                                   l r start select ps; sticks: --lx 255 --ly 0 --rx 128 --ry 128

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
 CMD_PUT_CLOSE, CMD_MKDIR, CMD_REMOVE, CMD_RMDIR, CMD_RENAME, CMD_POWER,
 CMD_LAUNCH, CMD_KILL, CMD_SCREEN, CMD_INPUT, CMD_TOUCH) = range(1, 19)
BUTTONS = {"select": 0x1, "l3": 0x2, "r3": 0x4, "start": 0x8, "up": 0x10, "right": 0x20, "down": 0x40,
           "left": 0x80, "l": 0x100, "r": 0x200, "l1": 0x400, "r1": 0x800, "triangle": 0x1000,
           "circle": 0x2000, "cross": 0x4000, "square": 0x8000, "ps": 0x10000}
POWER_ACTIONS = {"reboot": 0, "soft": 1, "poweroff": 2, "suspend": 3}

RX_SIZE_OLD = 4096             # the command buffer of vdb 0.2 (no "rx" in its status line)
DATA_CHUNK = 1024 * 1024       # largest reply frame any plugin version sends (0.3.9: 256 KB)
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


def logs(stamp=False, path=None):
    """Tail the ring; when the Vita leaves the bus (reboot, sleep, cable) wait
    for it to come back and attach again, so one recorder covers a whole
    session of reboots."""
    dev = None
    buf = b""
    out = open(path, "a") if path else None

    def emit(line):
        if stamp:
            line = time.strftime("%H:%M:%S ") + line
        print(line, flush=True)
        if out:
            out.write(line + "\n")
            out.flush()
    while True:
        if dev is None:
            while usb.core.find(idVendor=VID, idProduct=PID) is None:
                time.sleep(1)
            try:
                dev = open_device(0)
            except (usb.core.USBError, SystemExit):
                time.sleep(1)
                continue
            buf = b""
            print(f"vdb: attached {time.strftime('%H:%M:%S')}, waiting for log data (ctrl-c to stop)", file=sys.stderr)
        try:
            data = dev.read(EP_IN, 4096, timeout=2000)
        except usb.core.USBTimeoutError:
            continue
        except usb.core.USBError as e:
            print(f"vdb: usb error ({e}), waiting for the Vita to come back", file=sys.stderr)
            try:
                usb.util.dispose_resources(dev)
            except Exception:
                pass
            dev = None
            time.sleep(1)
            continue
        buf += bytes(data)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            emit(line.decode("utf-8", "replace"))


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


def put_bytes(b, data, dst, label=None):
    status, _ = b.simple(CMD_PUT_OPEN, path_arg(dst))
    check(status, f"put open {dst}")
    for i in range(0, len(data), b.put_chunk):
        b.send(CMD_PUT_DATA, data[i:i + b.put_chunk])
    status, rdata = b.simple(CMD_PUT_CLOSE)
    (written,) = struct.unpack("<Q", rdata[:8]) if len(rdata) >= 8 else (0,)
    check(status, f"put {dst}")
    if written != len(data):
        sys.exit(f"{dst}: wrote {written}, expected {len(data)}")


def cmd_install(b, vpk, title):
    import zipfile
    root = f"ux0:app/{title}/"
    status, _ = b.simple(CMD_STAT, path_arg(root + "eboot.bin"))
    if status < 0:
        sys.exit(f"install: {root}eboot.bin not found; install the title once with VitaShell first")
    t0 = time.time()
    total = 0
    dirs = set()
    with zipfile.ZipFile(vpk) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            name = info.filename.lstrip("/")
            parts = name.split("/")
            for i in range(1, len(parts)):
                d = "/".join(parts[:i])
                if d not in dirs:
                    b.simple(CMD_MKDIR, path_arg(root + d))  # exists already: fine
                    dirs.add(d)
            data = z.read(info)
            put_bytes(b, data, root + name)
            total += len(data)
            print(f"\r{name}: {len(data)} bytes            ", end="", file=sys.stderr, flush=True)
    dt = max(time.time() - t0, 1e-6)
    print(f"\rinstalled {vpk} over {root}: {total} bytes in {dt:.1f} s ({total / dt / 1e6:.1f} MB/s)      ", file=sys.stderr)


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


def write_png(path, width, height, rows_rgb):
    import zlib
    raw = b"".join(b"\x00" + r for r in rows_rgb)

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def cmd_screen(b, out=None):
    out = out or time.strftime("screen-%H%M%S.png")
    b.send(CMD_SCREEN)
    _, status, data = b.reply()
    check(status, "screen")
    width, height, pitch, fmt, pid, nbytes = struct.unpack("<6I", data[:24])
    buf = bytearray()
    while True:
        _, status, data = b.reply(timeout=15000)
        if not data:
            check(status, "screen")
            break
        buf += data
    if len(buf) != nbytes:
        sys.exit(f"screen: got {len(buf)} of {nbytes} bytes")
    # pixel format 0 is A8B8G8R8: bytes in memory are R, G, B, A
    rows = []
    for y in range(height):
        row = buf[y * pitch * 4:(y * pitch + width) * 4]
        rows.append(bytes(v for i, v in enumerate(row) if i % 4 != 3))
    write_png(out, width, height, rows)
    print(f"{out}: {width}x{height} (pitch {pitch}, format 0x{fmt:x}, pid 0x{pid:x})")


def cmd_input(b, args):
    buttons, hold = 0, 200
    sticks = {"lx": 0x80, "ly": 0x80, "rx": 0x80, "ry": 0x80}
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--hold":
            hold = int(args[i + 1]); i += 2
        elif a[2:] in sticks and a.startswith("--"):
            sticks[a[2:]] = max(0, min(255, int(args[i + 1]))); i += 2
        elif a in BUTTONS:
            buttons |= BUTTONS[a]; i += 1
        else:
            sys.exit(f"input: unknown '{a}'")
    payload = struct.pack("<I4BI", buttons, sticks["lx"], sticks["ly"], sticks["rx"], sticks["ry"], hold)
    status, _ = b.simple(CMD_INPUT, payload)
    check(status, "input")
    print(f"input: buttons 0x{buttons:x}, sticks {sticks}, held {hold} ms")


def cmd_touch(b, x0, y0, x1, y1, ms, what):
    payload = struct.pack("<4HI", x0, y0, x1, y1, ms)
    status, _ = b.simple(CMD_TOUCH, payload)
    if status == -6:
        sys.exit("touch: the plugin's touch hooks are not installed (taiHEN missing?)")
    check(status, "touch")
    print(f"{what}: ({x0},{y0}) -> ({x1},{y1}) over {ms} ms")


def opt_int(args, name, default):
    if name in args:
        i = args.index(name)
        v = int(args[i + 1])
        del args[i:i + 2]
        return v
    return default


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    cmd, rest = args[0], args[1:]
    if cmd == "logs":
        path = rest[rest.index("--file") + 1] if "--file" in rest else None
        logs(stamp="--ts" in rest, path=path)
        return 0
    b = Bridge()
    if cmd == "run" and 1 <= len(rest) <= 2:
        title = rest[0]
        if len(rest) == 2:
            cmd_install(b, rest[1], title)
        status, _ = b.simple(CMD_LAUNCH, path_arg(title))
        if status == -5:
            sys.exit("run: the shell companion is not running (vdb_shell.suprx under *main in config.txt)")
        check(status, f"launch {title}")
        print(f"{title} launched; tailing the log (ctrl-c to stop)", file=sys.stderr)
        usb.util.dispose_resources(b.dev)
        logs(stamp=True)
        return 0
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
    elif cmd == "install" and len(rest) == 2:
        cmd_install(b, rest[0], rest[1])
    elif cmd == "mkdir" and len(rest) == 1:
        check(b.simple(CMD_MKDIR, path_arg(rest[0]))[0], f"mkdir {rest[0]}")
    elif cmd == "rm" and len(rest) == 1:
        check(b.simple(CMD_REMOVE, path_arg(rest[0]))[0], f"rm {rest[0]}")
    elif cmd == "rmdir" and len(rest) == 1:
        check(b.simple(CMD_RMDIR, path_arg(rest[0]))[0], f"rmdir {rest[0]}")
    elif cmd == "mv" and len(rest) == 2:
        check(b.simple(CMD_RENAME, path_arg(rest[0]) + path_arg(rest[1]))[0], f"mv {rest[0]}")
    elif cmd == "launch" and len(rest) == 1:
        # a title id, or a full uri such as psgm:play?titleid=ABCD12345; the
        # shell companion (vdb_shell.suprx) starts it the way the shell does
        status, _ = b.simple(CMD_LAUNCH, path_arg(rest[0]))
        if status == -5:
            sys.exit("launch: the shell companion is not running (vdb_shell.suprx under *main in config.txt)")
        check(status, f"launch {rest[0]}")
        print(f"launch of {rest[0]} handed to the shell; the result shows up in the log stream")
    elif cmd == "kill" and len(rest) <= 1:
        pid = int(rest[0], 0) if rest else 0
        b.send(CMD_KILL, arg=pid & 0xFFFFFFFF)
        _, status, data = b.reply()
        (pid,) = struct.unpack("<i", data[:4]) if len(data) >= 4 else (0,)
        if status == -3:
            sys.exit(f"kill: pid 0x{pid:x} is the shell, refused")
        check(status, "kill")
        print("asked the shell to close the running app" if pid == 0 else f"killed pid 0x{pid:x}")
    elif cmd == "screen" and len(rest) <= 1:
        cmd_screen(b, *rest)
    elif cmd == "input" and rest:
        cmd_input(b, rest)
    elif cmd == "touch" and len(rest) >= 2:
        hold = opt_int(rest, "--hold", 100)
        x, y = int(rest[0]), int(rest[1])
        cmd_touch(b, x, y, x, y, hold, "touch")
    elif cmd == "swipe" and len(rest) >= 4:
        ms = opt_int(rest, "--ms", 400)
        cmd_touch(b, *(int(v) for v in rest[:4]), ms, "swipe")
    elif cmd == "unlock":
        cmd_touch(b, 930, 60, 200, 500, 600, "unlock swipe")
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
