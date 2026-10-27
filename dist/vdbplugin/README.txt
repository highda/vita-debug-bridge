vdb 0.1 (Vita debug bridge, MVP): streams a kernel log ring over USB.

Install (firmware 3.65, HENkaku/Enso, taiHEN):
1. Copy this folder to ux0:vdbplugin/  (so ux0:vdbplugin/vdb.skprx exists).
2. Add to ur0:tai/config.txt (ux0:tai/config.txt if that exists), under *KERNEL:
       ux0:vdbplugin/vdb.skprx
3. The file ux0:vdbplugin/usb_on makes the plugin take the USB port at
   boot (the Vita then shows up on the Mac as "VDB", not as a drive).
   Delete usb_on and reboot to get VitaShell's USB storage back.
4. Reboot. On the Mac:  vdb.py logs   (tails the ring; a heartbeat line
   every 5 s proves the pipe), vdb.py ping / status.

Recovery: hold L while the Vita boots to skip all taiHEN plugins, or
remove the config.txt line (ur0:tai/config.txt via VitaShell, or ux0 in a reader).
