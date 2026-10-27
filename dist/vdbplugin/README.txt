vdb 0.2.2 (Vita debug bridge): streams a kernel log ring over USB and moves
files on any device (ux0:, ur0:, uma0:, vs0:, os0: ...) while a game runs.

Install (firmware 3.65, HENkaku/Enso, taiHEN):
1. Copy this folder to ux0:vdbplugin/  (so ux0:vdbplugin/vdb.skprx exists).
2. Add to ur0:tai/config.txt (ux0:tai/config.txt if that exists), under *KERNEL:
       ux0:vdbplugin/vdb.skprx
3. The file ux0:vdbplugin/usb_on makes the plugin take the USB port once
   the shell is up (the Vita then shows up on the Mac as "VDB", not as a
   drive). Delete usb_on and reboot to get VitaShell's USB storage back,
   or press L + R + Start + Select to switch at any time.
4. Reboot. On the Mac:  vdb.py logs   (tails the ring; a heartbeat line
   every 5 s proves the pipe), vdb.py ping / status / ls ux0: / get / put,
   vdb.py reboot (or poweroff) to restart the Vita from the computer.

Recovery: hold L while the Vita boots to skip all taiHEN plugins, or
remove the config.txt line (ur0:tai/config.txt via VitaShell, or ux0 in a reader).
