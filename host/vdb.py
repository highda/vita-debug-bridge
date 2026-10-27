#!/usr/bin/env python3
"""vdb: host side of the Vita debug bridge (MVP).

  vdb.py logs            tail the Vita's log ring over USB
  vdb.py ping            ask the plugin for a pong (printed by `logs`)
  vdb.py status          ask for ring/uptime status (printed by `logs`)

Needs pyusb (pip install pyusb) and libusb (brew install libusb).
"""
import sys
import time

import usb.core
import usb.util

VID = 0x054C   # Sony, filled in by the Vita's USB stack
PID = 0x0DB6   # vdb
EP_IN = 0x81
EP_OUT = 0x02


def open_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit("no vdb device (Vita not connected, plugin idle, or usb_on flag missing)")
    try:
        dev.set_configuration()
    except usb.core.USBError as e:
        # already configured, or claimed: try to go on
        print(f"set_configuration: {e}", file=sys.stderr)
    usb.util.claim_interface(dev, 0)
    return dev


def cmd_send(dev, byte):
    dev.write(EP_OUT, byte, timeout=1000)


def logs(dev):
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


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    dev = open_device()
    cmd = sys.argv[1]
    if cmd == "logs":
        logs(dev)
    elif cmd == "ping":
        cmd_send(dev, b"P")
    elif cmd == "status":
        cmd_send(dev, b"S")
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
