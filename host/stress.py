#!/usr/bin/env python3
"""vdb stress test: a long random mix of real operations and malformed frames.

  python3 host/stress.py [--minutes 30] [--app VITASHELL] [--seed N] [--no-launch] [--no-reset]

Every operation is checked (file round trips by hash, every malformed frame
followed by a ping that must answer), the plugin's uptime is watched for
restarts, and a summary table is printed at the end. Files go to
ux0:data/vdb_stress/ and are removed. The app given with --app is started
and closed now and then (that makes the system take the USB port back,
which the plugin must retake); no input is ever sent to it. Keep-awake is
set to "screen" for the run and back to "off" at the end.
"""
import argparse
import hashlib
import os
import random
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usb.core  # noqa: E402
import usb.util  # noqa: E402
import vdb  # noqa: E402

DIR = "ux0:data/vdb_stress"
SIZES = [0, 1, 511, 512, 513, 4095, 4096, 65535, 65536, 65537, 262128, 262144, 1000003, 4 * 1024 * 1024]
# commands a malformed frame may carry: nothing that writes, powers off,
# launches, kills or presses buttons
SAFE_CMDS = [vdb.CMD_PING, vdb.CMD_STATUS, vdb.CMD_LIST, vdb.CMD_STAT, vdb.CMD_GET, vdb.CMD_PROCINFO, vdb.CMD_PEEK, 0, 26, 99, 0xFFFFFFFF]


class Failure(Exception):
    pass


_current = [None]


def connect(timeout=90):
    """A fresh bridge; the previous handle of this process is released first
    (it still holds interface 1, and a second claim is refused)."""
    if _current[0] is not None:
        try:
            usb.util.dispose_resources(_current[0].dev)
        except Exception:  # noqa: BLE001
            pass
        _current[0] = None
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        try:
            _current[0] = vdb.Bridge()
            return _current[0], time.time() - t0
        except (SystemExit, usb.core.USBError, IOError, ValueError) as e:
            last = e
            time.sleep(1)
    raise Failure(f"no bridge after {timeout} s ({last})")


def drain(b, ms=300):
    """Read and drop replies until the pipe stays quiet."""
    n = 0
    while True:
        try:
            b.dev.read(vdb.EP_DATA, vdb.RSP.size + vdb.DATA_CHUNK, timeout=ms)
            n += 1
        except usb.core.USBTimeoutError:
            return n


def get_bytes(b, path):
    b.send(vdb.CMD_GET, vdb.path_arg(path))
    _, status, data = b.reply()
    if status < 0:
        raise Failure(f"get {path}: 0x{status & 0xFFFFFFFF:08x}")
    (size,) = struct.unpack("<Q", data[:8])
    out = bytearray()
    while True:
        _, status, data = b.reply(timeout=15000)
        if not data:
            if status < 0:
                raise Failure(f"get {path}: 0x{status & 0xFFFFFFFF:08x}")
            break
        out += data
    if len(out) != size:
        raise Failure(f"get {path}: {len(out)} of {size} bytes")
    return bytes(out)


def status_fields(b):
    status, data = b.simple(vdb.CMD_STATUS)
    text = data.decode("utf-8", "replace")
    up = int(text.split("uptime ")[1].split()[0])
    dropped = int(text.split("dropped ")[1].split(",")[0])
    return up, dropped, text


# --- operations: each raises Failure (or a USB error) when something is wrong

def op_ping(b, ctx):
    status, _ = b.simple(vdb.CMD_PING)
    if status != 0:
        raise Failure(f"ping status {status}")


def op_roundtrip(b, ctx):
    size = random.choice(SIZES)
    data = os.urandom(size)
    path = f"{DIR}/f{random.randint(0, 9)}.bin"
    try:
        vdb.put_bytes(b, data, path)
    except SystemExit as e:
        raise Failure(f"put {size} bytes: {e}")
    back = get_bytes(b, path)
    if hashlib.sha256(back).digest() != hashlib.sha256(data).digest():
        raise Failure(f"round trip of {size} bytes differs")
    status, _ = b.simple(vdb.CMD_REMOVE, vdb.path_arg(path))
    if status < 0:
        raise Failure(f"rm {path}: 0x{status & 0xFFFFFFFF:08x}")
    ctx["bytes"] += 2 * size
    return f"{size} B"


def op_list(b, ctx):
    b.send(vdb.CMD_LIST, vdb.path_arg("ux0:data"))
    try:
        vdb.read_frames(b, "ls")
    except SystemExit as e:
        raise Failure(str(e))


def op_screen(b, ctx):
    try:
        vdb.cmd_screen(b, ctx["png"], quiet=True)
    except SystemExit as e:
        raise Failure(str(e))


def op_peek(b, ctx):
    b.send(vdb.CMD_PROCINFO, arg=0)
    try:
        text, _ = vdb.read_frames(b, "ps")
    except SystemExit as e:
        raise Failure(str(e))
    mods = [line.split() for line in text.decode("utf-8", "replace").splitlines() if line.startswith("M ")]
    if not mods:
        return "no modules"
    _, _, base, size = random.choice(mods)[:4]
    b.send(vdb.CMD_PEEK, struct.pack("<II", int(base, 16), min(int(size, 16), 4096)), arg=0)
    try:
        data, _ = vdb.read_frames(b, "peek")
    except SystemExit as e:
        raise Failure(str(e))
    if not data:
        raise Failure("peek returned nothing")


def op_profile(b, ctx):
    b.send(vdb.CMD_PROFILE, struct.pack("<II", 0, 1000), arg=1)
    _, status, _ = b.reply()
    if status < 0:
        raise Failure(f"profile start 0x{status & 0xFFFFFFFF:08x}")
    time.sleep(1)
    b.send(vdb.CMD_PROFILE, arg=0)
    t_stop = time.time()
    b.reply(timeout=30000)
    ctx.setdefault("stop_ms", []).append((time.time() - t_stop) * 1000)
    b.send(vdb.CMD_PROFILE, arg=2)
    try:
        text, _ = vdb.read_frames(b, "profile dump")
    except SystemExit as e:
        raise Failure(str(e))
    if not text.startswith(b"R "):
        raise Failure("profile dump without its header")


def op_kprintf(b, ctx):
    for arg in (1, 0):
        b.send(vdb.CMD_KPRINTF, arg=arg)
        _, status, _ = b.reply()
        if status < 0:
            raise Failure(f"kprintf {arg}: {status}")


def op_osd(b, ctx):
    for text in (f"vdb stress {ctx['n']}", ""):
        status, _ = b.simple(vdb.CMD_OSD, text.encode() + b"\0")
        if status < 0 and status != -6:
            raise Failure(f"osd: {status}")


def op_fuzz(b, ctx):
    kind = random.choice(["random", "short", "liar", "cmd", "huge_len"])
    if kind == "random":
        frame = os.urandom(random.randint(1, b.rx_size))
    elif kind == "short":
        frame = os.urandom(random.randint(1, vdb.HDR.size - 1))
    elif kind == "liar":   # the header claims more payload than the frame has
        payload = os.urandom(random.randint(0, 64))
        frame = vdb.HDR.pack(vdb.CMD_MAGIC, random.choice(SAFE_CMDS), len(payload) + random.randint(1, 1 << 20), random.getrandbits(32)) + payload
    elif kind == "huge_len":
        frame = vdb.HDR.pack(vdb.CMD_MAGIC, random.choice(SAFE_CMDS), 0xFFFFFFFF, random.getrandbits(32))
    else:                  # a safe command with a random argument and payload
        payload = os.urandom(random.randint(0, 512))
        frame = vdb.HDR.pack(vdb.CMD_MAGIC, random.choice(SAFE_CMDS), len(payload), random.getrandbits(32)) + payload
    if len(frame) % b.packet == 0 and len(frame) < b.rx_size:
        frame += b"\0"
    b.dev.write(vdb.EP_OUT, frame, timeout=vdb.TIMEOUT)
    time.sleep(0.05)
    drained = drain(b)
    op_ping(b, ctx)
    return f"{kind}, {drained} replies"


def op_launch_kill(b, ctx):
    status, _ = b.simple(vdb.CMD_LAUNCH, vdb.path_arg(ctx["app"]))
    if status == -5:
        return "no shell companion"
    if status < 0:
        raise Failure(f"launch: {status}")
    time.sleep(6)
    nb, t = connect()          # the app's start may have moved the port around
    ctx["b"] = nb
    nb.send(vdb.CMD_KILL, arg=0)
    nb.reply()
    time.sleep(4)
    nb2, t2 = connect()        # the exit gives the port to the system: retake
    ctx["b"] = nb2
    ctx["reconnects"].append(t + t2)
    return f"back in {t + t2:.1f} s"


def op_reset(b, ctx):
    try:
        b.dev.reset()
    except usb.core.USBError:
        pass  # the device re-enumerates under the reset; pyusb reports it gone
    time.sleep(1)
    nb, t = connect()
    ctx["b"] = nb
    ctx["reconnects"].append(t)
    return f"back in {t:.1f} s"


OPS = [  # (name, function, weight)
    ("ping", op_ping, 10), ("roundtrip", op_roundtrip, 25), ("list", op_list, 6), ("screen", op_screen, 6),
    ("peek", op_peek, 6), ("profile", op_profile, 4), ("kprintf", op_kprintf, 3), ("osd", op_osd, 3),
    ("fuzz", op_fuzz, 25), ("launch_kill", op_launch_kill, 2), ("reset", op_reset, 2),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--app", default="VITASHELL")
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--no-launch", action="store_true")
    ap.add_argument("--no-reset", action="store_true")
    ap.add_argument("--only", help="comma-separated operations to run (e.g. fuzz,ping)")
    ap.add_argument("--skip", help="comma-separated operations to leave out")
    a = ap.parse_args()
    random.seed(a.seed)
    ops = [o for o in OPS if not (a.no_launch and o[0] == "launch_kill") and not (a.no_reset and o[0] == "reset")]
    if a.only:
        ops = [o for o in ops if o[0] in a.only.split(",")]
    if a.skip:
        ops = [o for o in ops if o[0] not in a.skip.split(",")]
    names, funcs, weights = zip(*ops)
    b, _ = connect()
    b.simple(vdb.CMD_MKDIR, vdb.path_arg(DIR))
    b.send(vdb.CMD_AWAKE, arg=2)
    b.reply()
    up0, drop0, text0 = status_fields(b)
    print(f"seed {a.seed}, {a.minutes} min; start: {text0.strip()}", flush=True)
    ctx = {"b": b, "app": a.app, "bytes": 0, "reconnects": [], "n": 0,
           "png": os.path.join(tempfile.gettempdir(), "vdb_stress.png")}
    stats = {n: {"ok": 0, "fail": 0, "ms": []} for n in names}
    failures, restarts = [], 0
    last_up = up0
    prev, prev_detail = "start", None
    t_end = time.time() + a.minutes * 60
    while time.time() < t_end:
        ctx["n"] += 1
        i = random.choices(range(len(funcs)), weights)[0]
        name, fn = names[i], funcs[i]
        t0 = time.time()
        try:
            detail = fn(ctx["b"], ctx)
            stats[name]["ok"] += 1
            stats[name]["ms"].append((time.time() - t0) * 1000)
        except (Failure, usb.core.USBError, IOError, ValueError, struct.error) as e:
            import traceback
            where = [f"{fr.name}:{fr.lineno}" for fr in traceback.extract_tb(e.__traceback__) if "/usb/" not in fr.filename][-4:]
            e = f"{e} [{' < '.join(reversed(where))}]"
            stats[name]["fail"] += 1
            failures.append((time.strftime("%H:%M:%S"), name, f"{e} (after {prev})"))
            print(f"{time.strftime('%H:%M:%S')} FAIL {name}: {e} (after {prev}: {prev_detail})", flush=True)
            try:
                ctx["b"], t = connect()
                ctx["reconnects"].append(t)
            except Failure as e2:
                print(f"{time.strftime('%H:%M:%S')} GIVING UP: {e2}", flush=True)
                break
            detail = None
        prev, prev_detail = name, detail
        if ctx["n"] % 25 == 0:
            try:
                up, dropped, _ = status_fields(ctx["b"])
                if up < last_up:
                    restarts += 1
                    print(f"{time.strftime('%H:%M:%S')} PLUGIN RESTARTED (uptime {last_up} -> {up})", flush=True)
                last_up = up
                done = sum(s["ok"] + s["fail"] for s in stats.values())
                print(f"{time.strftime('%H:%M:%S')} {done} ops, {len(failures)} failures, {ctx['bytes'] / 1e6:.0f} MB moved, uptime {up} s, ring dropped {dropped - drop0}", flush=True)
            except Exception as e:  # noqa: BLE001
                print(f"{time.strftime('%H:%M:%S')} status failed: {e}", flush=True)
    try:
        ctx["b"].send(vdb.CMD_AWAKE, arg=0)
        ctx["b"].reply()
        ctx["b"].simple(vdb.CMD_RMDIR, vdb.path_arg(DIR))
        _, _, text1 = status_fields(ctx["b"])
    except Exception:  # noqa: BLE001
        text1 = "(no final status)"
    print("\n%-12s %6s %5s %9s %9s" % ("operation", "ok", "fail", "p50 ms", "max ms"))
    for n in names:
        ms = sorted(stats[n]["ms"])
        p50 = ms[len(ms) // 2] if ms else 0
        print("%-12s %6d %5d %9.0f %9.0f" % (n, stats[n]["ok"], stats[n]["fail"], p50, ms[-1] if ms else 0))
    rc = ctx["reconnects"]
    print(f"\n{ctx['bytes'] / 1e6:.0f} MB moved and verified; {len(rc)} reconnects"
          + (f" (median {sorted(rc)[len(rc) // 2]:.1f} s, max {max(rc):.1f} s)" if rc else "")
          + f"; plugin restarts {restarts}; failures {len(failures)}")
    if ctx.get("stop_ms"):
        st = sorted(ctx["stop_ms"])
        print(f"profile stop: median {st[len(st) // 2]:.0f} ms, max {st[-1]:.0f} ms, over 1 s: {sum(1 for x in st if x > 1000)} of {len(st)}")
    print(f"end: {text1.strip()}")
    for f in failures:
        print("  ", *f)
    return 1 if failures or restarts else 0


if __name__ == "__main__":
    sys.exit(main())
