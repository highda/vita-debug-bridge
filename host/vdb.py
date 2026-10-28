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
  vdb.py reset                     USB reset of the bridge device from this side (a stuck pipe:
                                   commands time out although the device is enumerated); stop
                                   any `logs` first, the reset needs the device to itself
  vdb.py launch ABCD12345          start an installed app through the shell (needs vdb_shell.suprx)
  vdb.py kill [pid]                kill a process (default: the one that owns the screen; never the shell)
  vdb.py screen [out.png]          grab the screen (default: screen-<time>.png)
  vdb.py touch X Y [--hold 100] [--back]   a finger on the front (or rear) panel, screen pixels 960x544
  vdb.py swipe X0 Y0 X1 Y1 [--ms 400] [--back]
  vdb.py kprintf on|off            mirror the kernel's own printf output into the log stream
  vdb.py awake off|standby|screen  keep-awake while the bridge runs: off (default, the Vita sleeps and charges as usual),
                                   standby (no automatic standby, the screen may go off), screen (nothing goes off)
  vdb.py ps [pid]                  a process (default: the one on screen): title, threads, modules
  vdb.py peek ADDR LEN [--pid P] [-o file]   read process memory (hex dump, or to a file)
  vdb.py poke ADDR HEXBYTES [--pid P]        write process memory, e.g. poke 0x81001000 00112233
  vdb.py profile [SECONDS] [--pid P] [--us 1000] [--raw file]
                                   sample every thread's program counter, resolve against the
                                   binaries in --build (default ./build) with
                                   the load bases the launcher logged, print the hot functions
  vdb.py autopsy [--elf f] [--all] pull the newest core dump and run vita-parse-core on it
  vdb.py unlock                    peel the lock screen (a swipe from the top right corner)
  vdb.py osd "text" | off          text drawn over every frame (lines split at \\n, 8 x 110)
  vdb.py osd status                the plugin's status overlay (USB state, bridge, battery; hotkey L+R+Start+Up)
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
CMD_AWAKE = 25
AWAKE_MODES = {"off": 0, "standby": 1, "screen": 2}
RSP_MAGIC = 0x52424456  # "VDBR"
HDR = struct.Struct("<IIII")   # cmd: magic, cmd, len, arg; rsp: magic, cmd, status(i), len
RSP = struct.Struct("<IIiI")

(CMD_PING, CMD_STATUS, CMD_LIST, CMD_STAT, CMD_GET, CMD_PUT_OPEN, CMD_PUT_DATA,
 CMD_PUT_CLOSE, CMD_MKDIR, CMD_REMOVE, CMD_RMDIR, CMD_RENAME, CMD_POWER,
 CMD_LAUNCH, CMD_KILL, CMD_SCREEN, CMD_INPUT, CMD_TOUCH, CMD_OSD, CMD_KPRINTF,
 CMD_PEEK, CMD_POKE, CMD_PROCINFO, CMD_PROFILE) = range(1, 25)
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
    # after a re-enumeration (the Vita retakes the port after an app launch)
    # the device can sit unconfigured, and every bulk write then fails with an
    # I/O error: make sure configuration 1 is selected
    # Only when none is active: a SET_CONFIGURATION makes the Vita run its
    # attach path, which cancels every transfer in flight, the other
    # process's log read included
    try:
        configured = dev.get_active_configuration() is not None
    except usb.core.USBError:
        configured = False
    if not configured:
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
            self.rx_size = int(text.rsplit(", rx ", 1)[1].split()[0].rstrip(",;"))
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


def cmd_screen(b, out=None, quiet=False):
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
    if not quiet:
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


def cmd_touch(b, x0, y0, x1, y1, ms, what, back=False):
    payload = struct.pack("<4HII", x0, y0, x1, y1, ms, 1 if back else 0)
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


def read_frames(b, what):
    """Collect the data frames of a streaming reply until the empty one."""
    out = bytearray()
    while True:
        _, status, data = b.reply(timeout=20000)
        if not data:
            check(status, what)
            return bytes(out), status
        out += data


def cmd_ps(b, pid):
    b.send(CMD_PROCINFO, arg=pid & 0xFFFFFFFF)
    text, _ = read_frames(b, "ps")
    for line in text.decode("utf-8", "replace").splitlines():
        kind, rest = line[0], line[2:]
        if kind == "P":
            pid_s, title = rest.split(" ", 1)
            print(f"process {pid_s} title {title}")
        elif kind == "T":
            thid, prio, status, name = rest.split(" ", 3)
            print(f"  thread {thid} prio {int(prio):3d} status 0x{int(status):x} {name}")
        elif kind == "M":
            name, base, size, path = rest.split(" ", 3)
            print(f"  module {name:24s} {base} {size:>8s} {path}")


def cmd_peek(b, pid, addr, length, out=None):
    b.send(CMD_PEEK, struct.pack("<II", addr, length), arg=pid & 0xFFFFFFFF)
    data, _ = read_frames(b, "peek")
    if out:
        open(out, "wb").write(data)
        print(f"{out}: {len(data)} bytes from 0x{addr:08x}")
        return
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexs = " ".join(f"{c:02x}" for c in chunk)
        asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print(f"{addr + off:08x}  {hexs:<48s} {asc}")


def cmd_poke(b, pid, addr, data):
    b.send(CMD_POKE, struct.pack("<I", addr) + data, arg=pid & 0xFFFFFFFF)
    _, status, rdata = b.reply()
    (written,) = struct.unpack("<I", rdata[:4]) if len(rdata) >= 4 else (0,)
    check(status, "poke")
    print(f"poked {written} bytes at 0x{addr:08x}")


class ModuleMap:
    """Where things are loaded: the launcher's own log for the game's .so
    files (mapped by its loader, not the kernel) and ps for kernel modules."""

    def __init__(self, b, build_dir):
        self.mods = []  # (base, end, name, elf, link_vaddr)
        self.build = build_dir
        # the game libraries, from the launcher's boot log
        try:
            b.send(CMD_GET, path_arg("ux0:data/boot.txt"))
            _, status, _ = b.reply()
            text = read_frames(b, "boot log")[0].decode("utf-8", "replace") if status == 0 else ""
        except SystemExit:
            text = ""
        import re
        seen = {}
        for m in re.finditer(r"^(\S+\.so): mapped text 0x([0-9a-f]+) size 0x([0-9a-f]+)", text, re.M):
            name, base, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
            seen[name] = (base, base + size)  # the last mapping wins (a relaunch)
        for name, (base, end) in seen.items():
            self.mods.append((base, end, name, self.find_elf(name), 0))
        # kernel modules of the process (the launcher is "eboot.bin")
        b.send(CMD_PROCINFO, arg=0)
        for line in read_frames(b, "ps")[0].decode("utf-8", "replace").splitlines():
            if line.startswith("M "):
                name, base, size, path = line[2:].split(" ", 3)
                base, size = int(base, 16), int(size, 16)
                elf, link = (None, 0)
                if path.endswith("eboot.bin"):
                    elf = self.find_elf("eboot.elf")
                    link = self.elf_text_vaddr(elf) if elf else 0
                self.mods.append((base, base + size, name, elf, link))
        self.mods.sort()

    def find_elf(self, name):
        if not self.build:
            return None
        for root, _, files in os.walk(self.build):
            if name in files:
                return os.path.join(root, name)
        return None

    @staticmethod
    def elf_text_vaddr(elf):
        import subprocess
        try:
            out = subprocess.run(["arm-vita-eabi-readelf", "-l", elf], capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split()
                if parts and parts[0] == "LOAD":
                    return int(parts[2], 16)
        except FileNotFoundError:
            pass
        return 0x81000000

    def locate(self, pc):
        for base, end, name, elf, link in self.mods:
            if base <= pc < end:
                return name, pc - base, elf, link
        return None, pc, None, 0


def cmd_profile(b, seconds, pid, us, build_dir, raw_out=None):
    import subprocess
    b.send(CMD_PROFILE, struct.pack("<II", pid & 0xFFFFFFFF, us), arg=1)
    _, status, _ = b.reply()
    check(status, "profile start")
    print(f"sampling pid {'on screen' if pid == 0 else hex(pid)} every {us} us for {seconds} s...", file=sys.stderr)
    time.sleep(seconds)
    b.send(CMD_PROFILE, arg=0)
    _, status, data = b.reply()
    check(status, "profile stop")
    b.send(CMD_PROFILE, arg=2)
    text, _ = read_frames(b, "profile dump")
    text = text.decode("utf-8", "replace")
    if raw_out:
        open(raw_out, "w").write(text)
    mm = ModuleMap(b, build_dir)
    threads, cur = [], None
    rounds = 0
    for line in text.splitlines():
        kind, rest = line[0], line[2:]
        if kind == "R":
            rounds = int(rest.split()[0])
        elif kind == "T":
            thid, samples, running, overflow, name = rest.split(" ", 4)
            cur = {"thid": thid, "samples": int(samples), "running": int(running), "overflow": int(overflow), "name": name, "pcs": {}}
            threads.append(cur)
        elif kind == "S" and cur is not None:
            parts = rest.split()
            hexes, count = [int(v, 16) for v in parts[:-1]], int(parts[-1])
            pc, lr, ra = (hexes + [0, 0, 0])[:3]
            cur["pcs"][(pc, lr, ra)] = cur["pcs"].get((pc, lr, ra), 0) + count
    print(f"{rounds} rounds, {len(threads)} threads, modules known: {len(mm.mods)}")
    threads.sort(key=lambda t: -t["running"])
    cache = {}

    SYSTEM = ("SceLibKernel", "SceDriverUser", "SceLibc", "SceSysmem", "SceKernelThreadMgr")

    def symbolize(pc, lr=0, ra=0):
        name, off, elf, link = mm.locate(pc)
        # a sample inside a system library's stub belongs to the function that called it
        if lr and name is not None and name.startswith(SYSTEM):
            lname, _, _, _ = mm.locate(lr)
            # lr in the stub as well: the return address the stub pushed on the stack
            if ra and lname is not None and lname.startswith(SYSTEM):
                rname, _, _, _ = mm.locate(ra)
                if rname is not None and not rname.startswith(SYSTEM):
                    return symbolize(ra) + f"  (in {name})"
            if lname is not None and not lname.startswith(SYSTEM):
                return symbolize(lr) + f"  (in {name})"
        if name is None:
            return f"0x{pc:08x}"
        key = (elf, off + link)
        if elf and key not in cache:
            try:
                out = subprocess.run(["arm-vita-eabi-addr2line", "-f", "-C", "-e", elf, f"0x{off + link:x}"], capture_output=True, text=True).stdout.split("\n")
                cache[key] = out[0] if out and out[0] and out[0] != "??" else None
            except FileNotFoundError:
                cache[key] = None
        func = cache.get(key)
        return f"{name}+0x{off:x}" + (f" {func}" if func else "")

    for t in threads:
        if t["samples"] == 0:
            continue
        pct_run = 100.0 * t["running"] / max(rounds, 1)
        if t["running"] == 0:
            continue
        print(f"\n{t['name']} ({t['thid']}): on a core {pct_run:.0f}% of the time ({t['running']} of {t['samples']} samples), {t['overflow']} dropped")
        # aggregate by function (or address when unresolved)
        agg = {}
        for (pc, lr, ra), count in t["pcs"].items():
            sym = symbolize(pc, lr, ra)
            agg[sym] = agg.get(sym, 0) + count
        for sym, count in sorted(agg.items(), key=lambda kv: -kv[1])[:12]:
            print(f"  {100.0 * count / max(t['running'], 1):5.1f}%  {sym}")


def cmd_autopsy(b, elf, all_dumps=False):
    import subprocess
    cores_dir = os.path.expanduser("~/code-projects/vita-logs/cores")
    os.makedirs(cores_dir, exist_ok=True)
    b.send(CMD_LIST, path_arg("ux0:data/"))
    text, _ = read_frames(b, "ls")
    dumps = []
    for line in text.decode("utf-8", "replace").splitlines():
        parts = line.split(" ", 4)
        if len(parts) == 5 and parts[4].startswith("psp2core-") and parts[4].endswith(".psp2dmp"):
            dumps.append((parts[2] + " " + parts[3], int(parts[1]), parts[4]))
    if not dumps:
        print("no core dumps in ux0:data/")
        return
    dumps.sort()
    for when, size, name in (dumps if all_dumps else dumps[-1:]):
        local = os.path.join(cores_dir, name)
        if not os.path.exists(local) or os.path.getsize(local) != size:
            print(f"pulling {name} ({size} bytes, {when})", file=sys.stderr)
            cmd_get(b, "ux0:data/" + name, local)
        parser = os.path.expanduser("~/code-projects/vita-refs/vita-parse-core/main.py")
        print(f"=== {name} ({when})")
        r = subprocess.run([sys.executable, parser, local, elf], capture_output=True, text=True)
        print(r.stdout[-6000:] if r.returncode == 0 else (r.stdout[-2000:] + r.stderr[-2000:]))


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    cmd, rest = args[0], args[1:]
    if cmd == "reset":
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is None:
            sys.exit("no vdb device on the bus")
        try:
            dev.reset()
        except usb.core.USBError as e:
            sys.exit(f"reset failed: {e} (another process holding the device, such as a running `logs`?)")
        time.sleep(2)
        print("reset done; the Vita re-attaches and queues a fresh receive")
        return 0
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
    elif cmd == "screen" and "--loop" in rest:
        # keep capturing into the same file (default screen.png) at --fps (default 2) until ctrl-c
        rest.remove("--loop")
        fps = opt_int(rest, "--fps", 2)
        out = rest[0] if rest else "screen.png"
        print(f"capturing to {out} at {fps} fps (ctrl-c to stop)", file=sys.stderr)
        try:
            while True:
                t0 = time.time()
                cmd_screen(b, out, quiet=True)
                time.sleep(max(0.0, 1.0 / fps - (time.time() - t0)))
        except KeyboardInterrupt:
            pass
    elif cmd == "screen" and len(rest) <= 1:
        cmd_screen(b, *rest)
    elif cmd == "input" and rest:
        cmd_input(b, rest)
    elif cmd == "touch" and len(rest) >= 2:
        back = "--back" in rest and (rest.remove("--back") or True)
        hold = opt_int(rest, "--hold", 100)
        x, y = int(rest[0]), int(rest[1])
        cmd_touch(b, x, y, x, y, hold, "rear touch" if back else "touch", back)
    elif cmd == "swipe" and len(rest) >= 4:
        back = "--back" in rest and (rest.remove("--back") or True)
        ms = opt_int(rest, "--ms", 400)
        cmd_touch(b, *(int(v) for v in rest[:4]), ms, "rear swipe" if back else "swipe", back)
    elif cmd == "ps" and len(rest) <= 1:
        cmd_ps(b, int(rest[0], 0) if rest else 0)
    elif cmd == "peek" and len(rest) >= 2:
        pid = opt_int(rest, "--pid", 0)
        out = rest[rest.index("-o") + 1] if "-o" in rest else None
        cmd_peek(b, pid, int(rest[0], 0), int(rest[1], 0), out)
    elif cmd == "poke" and len(rest) >= 2:
        pid = opt_int(rest, "--pid", 0)
        cmd_poke(b, pid, int(rest[0], 0), bytes.fromhex(rest[1]))
    elif cmd == "profile":
        pid = opt_int(rest, "--pid", 0)
        us = opt_int(rest, "--us", 1000)
        build = rest[rest.index("--build") + 1] if "--build" in rest else os.path.expanduser("./build")
        raw = rest[rest.index("--raw") + 1] if "--raw" in rest else None
        seconds = int(rest[0]) if rest and rest[0].isdigit() else 10
        cmd_profile(b, seconds, pid, us, build, raw)
    elif cmd == "autopsy":
        elf = rest[rest.index("--elf") + 1] if "--elf" in rest else os.path.expanduser("./build/eboot.elf")
        cmd_autopsy(b, elf, "--all" in rest)
    elif cmd == "awake" and len(rest) == 1 and rest[0] in AWAKE_MODES:
        b.send(CMD_AWAKE, arg=AWAKE_MODES[rest[0]])
        _, status, _ = b.reply()
        check(status, "awake")
        print({"off": "keep-awake off: the Vita sleeps and charges as without the plugin",
               "standby": "no automatic standby (the screen still goes off, the port stays up)",
               "screen": "no standby and no screen off"}[rest[0]])
    elif cmd == "kprintf" and len(rest) == 1 and rest[0] in ("on", "off"):
        b.send(CMD_KPRINTF, arg=1 if rest[0] == "on" else 0)
        _, status, _ = b.reply()
        check(status, "kprintf")
        print(f"kernel printf capture {rest[0]}")
    elif cmd == "osd" and len(rest) == 1:
        text = "" if rest[0] == "off" else "@status" if rest[0] == "status" else rest[0].replace("\\n", "\n")
        status, _ = b.simple(CMD_OSD, text.encode("utf-8", "replace") + b"\0")
        if status == -6:
            sys.exit("osd: the plugin's display hook is not installed")
        check(status, "osd")
        print("osd off" if not text else "status overlay on (also: hotkey L+R+Start+Up)" if text == "@status" else f"osd: {len(text.splitlines())} line(s)")
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
