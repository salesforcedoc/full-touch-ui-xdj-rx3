# Notes

The non-obvious facts behind [`BUILD.md`](BUILD.md). Anything not here can be worked out from the code.

## Stack

- Pioneer's GPL kernel 3.0.101 (NXP i.MX6 BSP) + Pioneer's initramfs + v1.20 root file system + Pioneer's
  unmodified `rbp` (md5 `4f2efcfc0c9e3f539289f863acfddcc6`), started by Pioneer's `apl_start` (`aplstart=on`).
- Why not Yocto / mainline: rbp's Vivante GPU userspace (4.6.9 build 9754) only works with the identical galcore,
  which only Pioneer's kernel has.
- Boot: SPL → mainline u-boot (ATAGs, machid 4412) → uImage with Pioneer's initramfs embedded → SD root.
  u-boot and Pioneer's `fw_printenv` share one env (MMC @ 0xC0000, 8 KB).

## Wandboard QuadPlus kernel fixes (in the patch)

Pioneer's kernel predates the QuadPlus. Symptoms and fixes:

| Symptom | Fix |
|---|---|
| hangs after "VFP support" | skip busfreq / dvfs-core on 6QP (Quad-only DDR code) |
| silent serial console | enable Pioneer's console output option (bring-up only) |
| reboots every ~2 min | u-boot: no watchdog autostart (RX3 userspace never pings it) |
| wrong timer speed, GPU hangs | report the 6QP as silicon rev 2.0 (fixes clock routing) |
| GPU 2D core wedges | `galcore.powerManagement=0` in bootargs; OpenVG core not registered |
| HDMI no signal | unblank fb0 at boot; enable the PRG0 clock on 6QP; panel DDC is on I2C2 |
| HDMI-audio crash | only register HDMI audio when enabled (audio goes to the FLX4) |

## Gotchas

- Stop rbp with `kill -9` only: SIGTERM makes the RX3 userspace reboot the system.
- `isolcpus=3`: rbp pins its audio thread to CPU 3 (real-time); keep the core free.
- Display: the panel takes CVT 1280×800@60; if the HDMI link drops, the driver falls back to 720p (watchdog in
  `wandboard.initd` restores 1280×800).
- Touch: the panel's USB HID touch needs hid-multitouch; DirectFB's linux_input must be disabled (it grabs it).
- USB: Pioneer's udev rules decide USB1/USB2 by hub port (`2-1.2` = USB1, `2-1.1` = USB2), mount the stick
  at `/media/usb<slot>/<kernelname>` and post `mount <dir>` to the kernel queue `/proc/udev_*`; rbp reads
  `/tmp` FIFOs instead (touchshim points its open of `/proc/udev_usb*` at them). A board with its own kernel
  has no `/proc/udev_*`: `usb-mount.sh` and a host udev rule do the mount and the message there, and
  `usb-bridge.sh` (debounced) stays the bridge for a board that does have the queue.
- A stick has to be mounted **read-write** for rbp to see its rekordbox library. rbp's DeviceSQL opens
  `PIONEER/rekordbox/export.pdb` and `exportExt.pdb` for *writing* (page manager and lock), so on a
  read-only mount that open fails and rbp falls back to browsing the raw filesystem: the stick appears in
  SOURCE with its true capacity and the browse screen lists the folders — but the category column has only
  FOLDER/REC instead of PLAYLIST/ARTIST/ALBUM/..., and no tracks. Nothing is logged anywhere: rbp's log has
  no line about the media at all, and the only visible clue is that rbp holds no fd on the stick while a
  working one shows `export.pdb` + `exportExt.pdb` open. Pioneer's own rule mounts `flush,rw,...`; ours did
  not, and that one word was the whole difference.
- A udev `RUN` program cannot mount anything you will see: `systemd-udevd` runs in its own mount namespace,
  a slave of the host's (`master:` on the entries in its `/proc/<pid>/mountinfo`), so the mount stays inside
  udevd and never reaches the chroot. Start the work through `systemd-run` (which `90-rx3-usb.rules` does)
  or `nsenter -t 1 -m`.
- `/dev/gpiodrv` is pin-addressed on the RX3: open, `lseek` to the pin, then a 1-byte read of an *input* pin or
  write of an *output* one. A port that stands in for it with a single FIFO breaks both halves — a read blocks
  until someone writes (the constructor's read then hangs `UiObjectManager::init()`), and every write lands in
  the one stream where any parked reader takes it as *its own* pin's level, so an LED or attenuator relay going
  off reads as a fault on another pin. The shim has to answer the constructor's read, answer the over-current
  pins, and swallow writes (see the table above).
- exFAT needs `fuse.ko` built from this kernel; udev runs before it loads, so init.d re-mounts missed sticks.
- DDJ-FLX4: has its own power and only reconnects after its USB port has been off ~10 s (init.d does that via
  `hubtool` on the MEGA4). Needs a keep-alive SysEx every 200 ms.
- MEGA4 hub must have its own 5 V supply, or the touch controller drops off and wedges.
- `/tmp` is a 512 KB tmpfs on the RX3 rootfs: keep logs small.

## What the shims supply (Pioneer's rbp on non-RX3 hardware)

| Without it | Cause | Shim fix |
|---|---|---|
| no UI timers, USB never registers | `PanelComPeerLinux::postMessage` waits for a panel-ready flag | touchshim sets the flag |
| no key works | `IKeyManager` waits for the panel CPUs' first key report | touchshim clears that mask |
| "USB Error. Remove the device." | over-current GPIOs 126/204 (active low) read 0 — on a board where `/dev/gpiodrv` is one shared FIFO, a write to some *other* pin (mixer attenuator relays, panel LEDs) reaches a parked reader as its own pin's level, so a written 0 is read as a fault | touchshim reads those two pins as 1 and swallows writes to a tracked gpiodrv fd |
| **nothing works — no touch, no panel** | `GpioManager`'s constructor read of `/dev/gpiodrv` never returns, so `UiObjectManager::init()` hangs and `startUp()` returns before `TouchPanel::openDevice()` | touchshim answers the *first* read per fd (level 1) and lets later reads block |
| decks never play | ALSA device choice depends on the board revision in `/proc/cpuinfo` | touchshim reports revision 0x700 |
| browse drag moves once | list scroll sends one step per touch | touchshim: 2-instruction in-memory change |
| no touch / panel / codec | RX3 hardware absent | touchshim (TSC2007, panel links), audioshim (codec → FLX4) |

Addresses for all of this are `#define`s at the top of each shim (rbp v1.20).

## On-screen buttons (uishim)

rbp draws with DirectFB and presents each frame with the primary surface's `Flip`. uishim wraps that one function
pointer and draws its buttons into the back buffer just before the real `Flip`, so they are part of every frame
(no overlay, no flicker). The current screen comes from rbp's `browseMode` (1 deck, 2 timer, 3 browse,
4 tag list, 5 INFO, 7 utility, 10 shortcut, 12 source, 13 playlist, 14 search); touches are offered to uishim
first by touchshim and only reach rbp if no button claims them. Taps send RX3 key codes through knobshim.
