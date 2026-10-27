# vdb · Vita debug bridge

## Massive thanks to

- **xerpi**, for [vitastick](https://github.com/xerpi/vitastick) and [vita-udcd-uvc](https://github.com/xerpi/vita-udcd-uvc): the only readable examples of a PS Vita acting as a USB device from a kernel plugin. vdb's USB driver skeleton follows their structure; the code here was rewritten, but the path was theirs.
- **The molecule / HENkaku team** (Yifan Lu, Davee, Proxima, xyz) for HENkaku and **taiHEN**, which make kernel plugins possible at all.
- **TheFloW** for kubridge, the model for exporting kernel functions to user apps, and for Enso.
- **The VitaSDK team and contributors** for the toolchain, `vita-headers` and `vita-libs-gen`.

## Fully AI-coded

Every line in this repository was written by **Claude Fable 5.1** (Anthropic), working in Claude Code with a human at the Vita pressing the buttons, reading `boot.txt` when the boot died, and deciding what to build. No line was typed by hand. The commit history is the honest trail, including the three boots that powered the Vita off before the USB takeover was moved past the shell's own USB startup.

## What it is

A taiHEN **kernel plugin** (`vdb.skprx`) that turns the PS Vita's USB port into a debug channel for a running game, plus a small **host script** (`vdb.py`). With it, the Vita shows up on the computer as a vendor USB device ("VDB", `054C:0DB6`) instead of a drive, and:

- **logs stream live over USB** while the game runs: the plugin keeps a 64 KB kernel log ring that any app can write to with one syscall (`vdb_write`); the host tails it. A game can mirror its own debug log into it, so loads, frame statistics, slow reads and crashes arrive on the Mac as they happen, no card round trips;
- **commands go the other way** on a second bulk pipe (today: ping and status);
- **hotkeys** work without any app: `L + R + Start + Select` toggles the bridge (take the USB port / give it back to the system), `L + R + Start + Up` writes a status line to `ux0:vdbplugin/boot.txt`.

The MVP is log streaming. The roadmap, in the order each piece removes a manual step from a hardware debugging loop: file push/pull over the same pipe, VPK install and app launch through a user-side companion, controller and touch injection, screen capture (hardware JPEG, as vita-udcd-uvc does), and an on-screen status display.

## Why

Developing on real hardware with only a memory card for feedback means: play, quit, switch VitaShell to USB mode, copy logs, read, rebuild, copy, reboot. Each round costs minutes and loses the moment of the bug. The bridge keeps the cable in and the game running.

## Install (firmware 3.65, HENkaku/Enso, taiHEN)

1. Copy `dist/vdbplugin/` to `ux0:vdbplugin/` (it holds `vdb.skprx`, an empty `usb_on` flag and a README).
2. Add `ux0:vdbplugin/vdb.skprx` under `*KERNEL` in `ur0:tai/config.txt` (or `ux0:tai/config.txt` if that is what your setup uses). On an SD2Vita setup put it after `storagemgr.skprx`.
3. Reboot. About 25 s after boot, once the shell's own USB driver is up, the plugin takes the USB port.
4. On the computer (needs libusb and pyusb): `python3 host/vdb.py logs`, then `ping` or `status` from another terminal.

`usb_on` is the switch: delete it and reboot to get normal USB (VitaShell storage, Content Manager) back; the plugin stays loaded but idle, and the hotkey can still switch it on. Recovery from a bad plugin build: hold **L** while the Vita boots to skip all taiHEN plugins.

## Using it from your own app

Link `user/stubs/libVDB_365_stub_weak.a`, include `user/vdb_user.h`, check that the module is loaded (`_vshKernelSearchModuleByName( "VDB", buf ) >= 0`) and call `vdb_write( text, len )`. The weak stub keeps the plugin optional.

## Build

VitaSDK: `make` in `skprx/` builds `vdb.skprx`; `vita-libs-gen user/vdb_nids.yml user/stubs && make -C user/stubs` rebuilds the user stub library.

## Lessons the hard way

- Taking the USB port during boot (at 13 s) powered the Vita off, three boots in a row: SceShell brings up its own USB services afterwards and the two collide. Wait for the system's MTP driver to be activated (`ksceUdcdWaitState`), then take over; or do it on a hotkey.
- Set every flag a kernel thread reads *before* starting the thread; ours raced `module_start` and silently never activated.
- A verdict file (`boot.txt`) written at each step is the only debugger you have for the plugin that is itself the debugger.

## License

MIT, see `LICENSE`.
