# vdb · Vita debug bridge

## Massive thanks to

- **xerpi**, for [vitastick](https://github.com/xerpi/vitastick) and [vita-udcd-uvc](https://github.com/xerpi/vita-udcd-uvc): the only readable examples of a PS Vita acting as a USB device from a kernel plugin. vdb's USB driver skeleton follows their structure; the code here was rewritten, but the path was theirs.
- **The molecule / HENkaku team** (Yifan Lu, Davee, Proxima, xyz) for HENkaku and **taiHEN**, which make kernel plugins possible at all.
- **TheFloW** for kubridge, the model for exporting kernel functions to user apps, and for Enso.
- **devnoname120** for [vitacompanion](https://github.com/devnoname120/vitacompanion): starting and closing apps from a plugin inside SceShell, through the shell's own launch-by-URI, is its way; the kernel-side alternative took the system down.
- **xerpi** again, for [ds4vita](https://github.com/xerpi/ds4vita): the touch injection hooks (which kernel functions, which NIDs, how a report is patched in) follow it.
- **Electry** for [PSVshell](https://github.com/Electry/PSVshell), the model for drawing an overlay from a kernel plugin, and **Daniel Hepper** for the public domain [font8x8](https://github.com/dhepper/font8x8) the overlay is drawn with.
- **xyz and the vita-parse-core authors** for the core dump parser the autopsy runs.
- **The VitaSDK team and contributors** for the toolchain, `vita-headers` and `vita-libs-gen`.

## Fully AI-coded

Every line in this repository was written by **Claude Fable 5.1** (Anthropic), working in Claude Code with a human at the PC and Vita deciding on the direction and testing during development. No line was typed by hand. The commit history is the honest trail, including the three boots that powered the Vita off before the USB takeover was moved past the shell's own USB startup.

## What it is

A taiHEN **kernel plugin** (`vdb.skprx`) that turns the PS Vita's USB port into a debug channel for a running game, plus a small **host script** (`vdb.py`). With it, the Vita shows up on the computer as a vendor USB device ("VDB", `054C:0DB6`) instead of a drive, and:

- **logs stream live over USB** while the game runs: the plugin keeps a 64 KB kernel log ring that any app can write to with one syscall (`vdb_write`); the host tails it. A game that mirrors its debug log into it has its loads, frame statistics, slow reads and crashes arrive on the computer as they happen, no card round trips;
- **files move over the same cable** while the game runs: `vdb.py ls / stat / get / put / mkdir / rm / rmdir / mv` on any path the kernel can see (`ux0:`, `ur0:`, `uma0:`, `imc0:`, `vs0:`, `os0:` and the rest; the system partitions are yours to break). The commands go on a second bulk pipe, the replies and file data come back on a third, so no app on the Vita is involved and no USB mode switch is needed;
- **the Vita reboots or powers off from the computer**: `vdb.py reboot` (cold; `reboot soft`, `poweroff`, `suspend` too). With `put` over `ux0:vdbplugin/vdb.skprx` the plugin replaces itself and reboots into the new build without a hand on the Vita;
- **apps start and stop from the computer**: `vdb.py launch ABCD12345` (an installed title id, or a `psgm:` uri) and `vdb.py kill` (the running app; `kill <pid>` for a specific process, the shell is refused). These go through a small user-side companion inside SceShell, `vdb_shell.suprx`, which calls the shell's own launch and close functions: launching from the kernel plugin itself takes the system down. Together with `put` into `ux0:app/<title>/` this replaces the VPK round trip for a game under development;
- **the screen comes over the cable**: `vdb.py screen shot.png` copies the framebuffer of the process on screen out of its memory (960x544, about 0.3 s) and writes a PNG;
- **buttons, sticks and touch can be injected**: `vdb.py input cross --hold 200`, `vdb.py input --lx 255 --hold 1000` through the kernel's own controller emulation; `vdb.py touch X Y`, `vdb.py swipe X0 Y0 X1 Y1 --ms 400` (add `--back` for the rear panel) through taiHEN hooks on the kernel touch readers (ds4vita's method), and `vdb.py unlock` peels the lock screen that every reboot leaves behind;
- **every printf on the system** can be mirrored into the stream: `vdb.py kprintf on` registers for the kernel's debug putchar, which receives the kernel's own messages (module loads, missing libraries) and the `sceClibPrintf` output of every user process. Any homebrew's debug prints arrive on the Mac without the app knowing about the bridge;
- **an on-screen display**: `vdb.py osd "fps 30\\nline two"` draws up to six lines of text over every frame the app presents (a hook on the display driver's SetFrameBuf, PSVshell's method), `vdb.py osd off` removes it. An app can write its own lines through the `vdb_osd` syscall, for stats without a cable;
- **a sampling profiler, no instrumentation**: `vdb.py profile 10` suspends each thread of the process on screen for a few microseconds every millisecond, reads its program counter and return address (the kernel's debugger interface), and resolves the counts on the Mac against the binaries, using the load bases the app logged and `addr2line`. Hot functions per thread, in a real play session, without rebuilding anything;
- **crash autopsy in one command**: `vdb.py autopsy` pulls the newest core dump from `ux0:data` and runs vita-parse-core on it, so a crash on the device becomes threads, registers and a symbolized stop address on the Mac;
- **memory of a running process**: `vdb.py peek ADDR LEN`, `vdb.py poke ADDR HEXBYTES`, and `vdb.py ps` for the process on screen (title, threads, modules with their bases);
- **charging and sleep are the firmware's, not the plugin's.** SceUdcd owns USB charging for whatever device sits behind its controller driver: it tells Syscon the USB state on every bus event, asks ScePower for USB charging from the "USB Power Supply" system setting, rewrites the descriptor's power fields from it, and keeps the console from auto-standby while a host charges it (that is stock behaviour: a Vita connected to a PC turns its screen off on the power button and keeps charging). The plugin therefore does none of that any more (0.4.3 to 0.6.12 did, and wrongly). A host port gives 500 mA, full stop; with the screen on the battery gains about 6 mAh a minute, with the screen off the whole 500 mA go in. Keep-awake is a host choice: `vdb awake off` (default) leaves the Vita exactly as without the plugin, `awake standby` only cancels the automatic standby, `awake screen` also keeps the screen on. `status` shows the battery and ScePower's real charging status (its "is charging" flag is false on a host port by design);
- **hotkeys** work without any app: `L + R + Start + Select` toggles the bridge (take the USB port / give it back to the system), `L + R + Start + Up` writes a status line to `ux0:vdbplugin/boot.txt`.

The log pipe and the command pipe are on separate USB interfaces, so a log tail in one terminal keeps running while files move in another.

The roadmap, in the order each piece removes a manual step from a hardware debugging loop: VPK install and app launch through a user-side companion, controller and touch injection, screen capture (hardware JPEG, as vita-udcd-uvc does), and an on-screen status display.

## Quick start

```
python3 host/vdb.py logs --ts                 # live log, one line per event, timestamped
python3 host/vdb.py run ABCD12345 game.vpk    # write the build over the installed app, start it, tail the log
python3 host/vdb.py screen                    # screen-HHMMSS.png
python3 host/vdb.py input cross               # press X
python3 host/vdb.py kill                      # close the app, back to the home screen
python3 host/vdb.py reboot                    # cold reboot; `unlock` peels the lock screen afterwards
```

`host/vdb` is a one-line launcher for the same script (symlink it into your PATH; `VDB_PYTHON` picks the interpreter that has pyusb).

| Command | What it does |
|---|---|
| `logs [--ts] [--file f]` | tail the log ring; reattaches by itself after a reboot or a cable pull |
| `ping`, `status` | round trip; version, ring use, uptime, frame size |
| `ls`, `stat`, `get`, `put`, `mkdir`, `rm`, `rmdir`, `mv` | files on any device the kernel sees (`ux0:`, `ur0:`, `uma0:`, `vs0:`, ...) |
| `install game.vpk TITLE` | a VPK's files written over `ux0:app/TITLE/` (the title must exist once) |
| `launch TITLE`, `kill [pid]` | start an app, close the running app (through the shell companion) |
| `run TITLE [game.vpk]` | install, launch, tail |
| `screen [out.png]` | the framebuffer of the process on screen, as a PNG |
| `input ...`, `touch X Y`, `swipe X0 Y0 X1 Y1`, `unlock` | buttons, sticks, a finger (front or `--back`), the lock screen peel |
| `osd "text"`, `osd off` | the on-screen display |
| `kprintf on`, `kprintf off` | the kernel's and every app's printf output into the stream |
| `awake off`, `awake standby`, `awake screen` | keep-awake while the bridge runs: off (default, the Vita sleeps and charges as usual), no automatic standby, or no standby and no screen off |
| `ps`, `peek`, `poke` | the process on screen: threads and modules; its memory, read and written |
| `profile [seconds]` | a sampling profile of the process on screen, resolved to functions |
| `autopsy` | the newest core dump, parsed |
| `reboot [soft]`, `poweroff`, `suspend` | power |

## Protocol

Little-endian frames. Host to Vita (EP2 OUT, one USB transfer each, at most the plugin's receive buffer, 64 KB; the host reads the size from the `rx` field of the status reply before sending anything): `magic "VDB1", cmd, len, arg` (4 x u32) followed by the payload; paths are NUL-terminated, `rename` carries two. Vita to host (EP3 IN): `magic "VDBR", cmd, status (i32, SCE error when negative), len` followed by the payload. `get` answers with the 8-byte size, then 32 KB data frames, then an empty frame; `list` streams text lines (`d|f size yyyy-mm-dd hh:mm:ss name`) and ends with an empty frame whose status is the entry count; `put` is open, data frames (no replies, USB flow control does the pacing), close (status plus bytes written); `power` (cmd 13, arg 0 cold reboot, 1 soft reboot, 2 power off, 3 suspend) replies first and acts 300 ms later. `launch` (14, payload path and NUL-separated arguments, reply the pid), `kill` (15, arg pid or 0 for the screen owner), `screen` (16, reply a 24-byte header: width, height, pitch, pixel format, pid, bytes, then the raw rows in 32 KB frames, then an empty frame), `input` (17, payload buttons u32, four stick bytes, hold ms), `touch` (18, payload x0 y0 x1 y1 as u16 screen pixels, ms u32: one finger moving linearly), `osd` (19, payload text), `kprintf` (20, arg 1 or 0), `peek` (21, arg pid, payload addr and len, data frames then an empty one), `poke` (22, arg pid, payload addr and bytes), `procinfo` (23, text lines), `profile` (24, arg 1 start with payload pid and interval, 0 stop, 2 dump as text lines `S pc lr ra ra2 count`: the return register and the first two Thumb return addresses on the sampled stack, so a sample in a system stub is attributed through small helpers to the function that matters), `awake` (25, arg 0 off, 1 no automatic standby, 2 no standby and no screen off). The touch payload may carry a fifth u32, the panel (0 front, 1 back). The companion syscalls are `vdb_shell_poll` and `vdb_shell_result` (library VdbForUser). One frame is in flight at a time on the Vita side, so a transfer needs no buffering beyond one chunk; the kernel side is about 170 KB of static memory in all. Measured on a Slim with an SD2Vita card: pull 9 MB/s, push 5.5 to 6 MB/s to the card with 256 KB frames (a 21 MB file in 3.5 s; 8.4 MB/s to ur0: the card write is the limit, and bigger writes help it). The two receive buffers live in a physically contiguous uncached kernel block, which the USB controller needs; a buffer in the module's own .bss is not reliably reachable.

## Why

Developing on real hardware with only a memory card for feedback means: play, quit, switch VitaShell to USB mode, copy logs, read, rebuild, copy, reboot. Each round costs minutes and loses the moment of the bug. The bridge keeps the cable in and the game running.

## Install (firmware 3.65, HENkaku/Enso, taiHEN)

1. Copy `dist/vdbplugin/` to `ux0:vdbplugin/` (it holds `vdb.skprx`, an empty `usb_on` flag and a README).
2. Add `ux0:vdbplugin/vdb.skprx` under `*KERNEL` in `ur0:tai/config.txt` (or `ux0:tai/config.txt` if that is what your setup uses). On an SD2Vita setup put it after `storagemgr.skprx`. For `launch` and `kill`, also add `ux0:vdbplugin/vdb_shell.suprx` under `*main`, after `henkaku.suprx`.
3. Reboot. About 25 s after boot, once the shell's own USB driver is up, the plugin takes the USB port.
4. On the computer (needs libusb and pyusb): `python3 host/vdb.py logs`, then `ping`, `status`, `ls ux0:` or `get ux0:data/some.log` from another terminal.

`usb_on` is the switch: delete it and reboot to get normal USB (VitaShell storage, Content Manager) back; the plugin stays loaded but idle, and the hotkey can still switch it on. Recovery from a bad plugin build: hold **L** while the Vita boots to skip all taiHEN plugins.

## Using it from your own app

Link `user/stubs/libVDB_365_stub_weak.a`, include `user/vdb_user.h`, check that the module is loaded (`_vshKernelSearchModuleByName( "VDB", buf ) >= 0`) and call `vdb_write( text, len )`. The weak stub keeps the plugin optional.

## Build

VitaSDK: `make` in `skprx/` builds `vdb.skprx` (main.c: thread and module entry; usb.c: the device and its transfers; commands.c: the protocol; log.c: ring, boot notes, kernel printf; input.c: pad and touch; osd.c: the display), `make` in `shell/` builds `vdb_shell.suprx`; `vita-libs-gen user/vdb_nids.yml user/stubs && make -C user/stubs` rebuilds the user stub library.

## Known caveats

- **Standby and the cable.** While a host is attached and USB Power Supply is on, the firmware keeps the Vita from automatic standby and turns the power button into "screen off" (SceUdcd's own doing, the same as a stock Vita on a PC): the bridge stays up and the whole 500 mA go into the battery, which is the charging pause you want between sessions. Standby happens only with the cable out. Plugging the cable into a sleeping Vita triggers a charge wake, a partial one: screen off, back to standby after the auto-standby interval unless something keeps it awake. Set the automatic standby to several minutes (Settings > Power Save Settings), one minute is too short for the port to come back, or wake it with the power button.
- **500 mA.** A host port grants 500 mA and nothing on the Vita asks for more; a game draws more than that. Charge with the screen off, or from a charger.
- **The charging indicator** stays off on a host port (ScePower's "is charging" needs Syscon's "power online"); `status` shows the real charging status.
- **VitaShell's USB mode** is unavailable while the bridge holds the port: delete `ux0:vdbplugin/usb_on` and reboot, or press L+R+Start+Select to hand the port back.
- **The status overlay** (L+R+Start+Up, or `osd status`) shows every marker the plugin has when the Mac sees nothing: USB state decoded, bridge and host state, the suspend window, the last receive error and the retake counters.

## Lessons the hard way

- **Never touch the UDCD stack while the system suspends.** At standby the USB service stops every controller; a receive then fails with "bus driver not started" (0x80243007), and a plugin that reacts by retaking the port calls activate and stop into a suspending stack, which fails (0x80243002, 0x8024300E) and, retried every second, keeps the Vita from ever sleeping or charging: 969 retakes in one night, battery to zero, USB dead until a cold boot. A system event handler (`ksceKernelRegisterSysEventHandler`) marks the window; the retake waits it out and backs off on failure. Nor on a cable-out: a receive then fails with the same error, and 0.6.9 to 0.7.0 answered by deactivating the device in the middle of the controller's cable-out handling, which left the controller stuck "in progress" (0x80243006) for everybody, VitaShell included, until a cold boot. The only reasons to retake are the system activating its own device (an app launch) or a device left deactivated with the controller up and the cable in. Also: `ksceUdcdGetDrvState("USBDeviceControllerDriver")` returns -1, the controller is not a class driver; read bit 0x2000 of `ksceUdcdGetDeviceState` instead.

- A dead USB cable looks exactly like a dead plugin from the Mac. The plugin's notes in `boot.txt` tell them apart: a thread that keeps writing status lines and a boot whose MTP wait times out mean no cable. VitaShell's USB mode is the other test, it is the system's own stack.
- Re-queuing a UDCD receive request object from its own completion is refused now and then (0x80243007); with no receive outstanding every host write fails with an I/O error although the device is enumerated. One request object per buffer, a retry, and a watchdog that re-queues from the thread tick. The host's `reset` re-attaches a stuck device meanwhile.

- Taking the USB port during boot (at 13 s) powered the Vita off, three boots in a row: SceShell brings up its own USB services afterwards and the two collide. Wait for the system's MTP driver to be activated (`ksceUdcdWaitState`), then take over; or do it on a hotkey.
- Set every flag a kernel thread reads *before* starting the thread; ours raced `module_start` and silently never activated.
- A verdict file (`boot.txt`) written at each step is the only debugger you have for the plugin that is itself the debugger.

## License

MIT, see `LICENSE`.
