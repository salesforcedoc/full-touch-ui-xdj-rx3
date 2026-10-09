# Porting

What changes on another board, screen or controller. Details can be worked out from the code and
[`NOTES.md`](NOTES.md).

> You need your own legally obtained copy of the XDJ-RX3 firmware and must follow its license; this repository
> does not provide it or say where to get it. Nothing here bypasses copy protection, licensing or activation
> checks — the shims only stand in for RX3 hardware. See the disclaimer in the [README](../README.md).

## Requirements

- **CPU**: runs 32-bit ARM (armel, ARMv7 + VFP) programs; 64-bit ARM boards need kernel 32-bit support
  (`CONFIG_COMPAT`). 4 cores preferred (rbp pins audio to CPU 3; use `isolcpus=3`).
- **Display**: `/dev/fb0` at 1280×800, 16-bit. Other sizes or rotation need scaling/rotation code (not written).
- **Kernel features**: USB audio + MIDI (controller), hid-multitouch (touch), FUSE (exFAT), fbdev.

## Two routes

**A. Pioneer's kernel (i.MX6 boards only)** — keeps GPU acceleration. Port your board's file into Pioneer's
3.0.101 kernel (pinmux, Ethernet PHY, HDMI DDC, USB power, SD); QuadPlus boards also need the fixes in
`NOTES.md`.

**B. Your board's own kernel + chroot (any ARM board)** — no GPU (rbp's Vivante libraries need Pioneer's
galcore). `rx3-rootfs/route-b.sh` does all of this and starts rbp; verified on a Raspberry Pi 4 (4 KB pages,
32-bit EL0 via `CONFIG_COMPAT`) with the v1.20 rootfs, UI up at 1280×800 in software mode. In the RX3 root file
system as a chroot:

1. Bind-mount `/dev /proc /sys /tmp`.
2. FIFOs (not regular files) for `/dev/subucom_spi1.0 subucom_spi2.0 subucom_spi_rdy3.0 subucom_spi_rdy4.0 hidg0
   gpiodrv`; regular files for `/dev/printkdrv0 tsc2007_2-0048`; no `/dev/paudiog0`. Pioneer's initramfs makes
   these on an RX3 and a chroot never runs it, so make them yourself. rbp lseeks the FIFOs and logs
   `lseek: Illegal seek` either way, which is harmless.
3. Block `/dev/mem` (it would poke i.MX6 registers). A regular empty file bound over it is enough: rbp comes up
   without the `mmap(MAP_SHARED, fd=-1)` workaround this step used to call for.
4. DirectFB software mode — `system=fbdev`, `no-hardware`, `disable-module=gal`, `disable-module=linux_input`,
   `no-linux-input-grab` in `/usr/etc/directfbrc`. Stub `libg2d` (album-art scaling only).
5. `/tmp/udev_usb1/2`, `udev_usbctn1/2` FIFOs, which rbp opens and waits on for `mount <dir>` / `umount
   <dir>`. Pioneer's rules feed them from the kernel queue `/proc/udev_*`, which your board's kernel has
   not: there `route-b.sh` installs a host udev rule and `usb-mount.sh`, which mount a stick into the chroot
   at `/media/usb<slot>/<kernelname>` — read-write, which is what rbp's database needs (see the USB sticks
   bullet) — and post the same message. The rule runs the helper through `systemd-run`, because
   systemd-udevd's mount namespace is a slave of the host's, so a mount made by a udev `RUN` program stays
   inside udevd and never reaches the chroot.
6. `localhost` must resolve; remove stale `/tmp/guard_LocalDBServer /tmp/req_LocalDBServer` before starting.
7. Start `edb_streamd` with `EDB_BIN=/usr/bin`, then `rbp -r` from `/root/pdj`; shims via the chroot's
   `/etc/ld.so.preload`.

fb0 must be 1280×800×16 before rbp starts: it lays out 1280×800 and DirectFB keeps whatever mode the kernel
booted with, so anything else leaves the UI in a corner of a larger screen. Where the fbdev is DRM emulation
(vc4 on a Pi) `fbset` cannot change the mode — set it on the kernel command line instead, e.g.
`video=HDMI-A-1:1280x800@60`, plus `isolcpus=3` so rbp's audio thread gets its own core as on Pioneer's kernels.

Don't run `/root/pdj/apl_start`: it is Pioneer's own board bring-up. It would `modprobe g_pmulti` (the RX3's
USB-B audio gadget), mount the UBIFS `settings`/`gui` partitions, reset the Wandboard's USB hub through the
MEGA4, and set the clock to 2021-01-01 — and it starts rbp only if `fw_printenv` reports `aplstart=on`.


## Per device

- **Touch screen**: touchshim auto-detects evdev and takes the digitizer's range from the panel itself (`ABS_X/Y`
  min/max — they are not a constant: 1024×600 on an MPI7002, 1920×1080 on a `TSTP CTouch`). `TSC_RAW_*` overrides the
  range, `TSC_OFF_X/Y` the offset. Calibrate with `touch /tmp/uishim.cal` (9 targets, taps logged with
  `UISHIM_TOUCH_LOG=1`). If `/dev/gpiodrv` is a writer-less fifo rather than a char device, touchshim must answer
  `GpioManager`'s constructor read or `UiObjectManager::init()` hangs and the whole touch pipeline never starts
  (see the table in `NOTES.md`).
- **Controller**: audioshim expects `hw:DDJFLX4,0`, 4 ch (master 1/2, headphones 3/4); knobshim's `build_maps()`
  maps FLX4 MIDI to RX3 keys, plus the FLX4's keep-alive, jog resolution and LEDs. Key codes: the ones used
  are `#define`s in `knobshim-flx4.c` / `uishim-rx3.c`; rbp v1.20 lists all of them (with names) in
  `ui::KeyInput::keyCodeAsText()`. Send a key with knobshim's inject file: `printf '0x0202 9 1\n' > /tmp/flx4-inject`.
- **USB sticks**: `usb-mount.sh` (installed by `route-b.sh` as a host udev rule) does what Pioneer's own
  udev rule does on the RX3 — mount the stick at `/media/usb<slot>/<kernelname>` inside the chroot, then
  tell rbp. USB1 first, USB2 if USB1 is taken; more than two are ignored. Mounted **read-write**, as
  Pioneer's rule does: rbp's DeviceSQL opens `PIONEER/rekordbox/export.pdb` for writing, and on a
  read-only mount that open fails, after which rbp quietly browses the raw filesystem instead — the stick
  still shows in SOURCE with its capacity, but with no playlists, tracks or artwork. `RX3_USB_RO=1` mounts
  read-only for a data-only stick, with that loss. vfat and exfat (through the board kernel's own module —
  a stock board has no `mount.exfat-fuse`); HFS+ sticks are skipped, as the README already lists HFS+ as
  unsupported. `route-b.sh usb` shows what is mounted, read-write or not, and whether rbp has registered it
  (`DET_USB1`). The UI's EJECT unmounts the stick itself that way (rbp does the unmount); no udev event
  follows until you replug, so the bridge does not bring it back on its own — as on the RX3.
- **Board signals**: the over-current GPIO fix and the cpuinfo board revision (see `NOTES.md`) are needed on any
  board; with no `/dev/gpiodrv` the GPIO fix simply isn't used. Where the node is a FIFO stand-in rather than
  Pioneer's pin-addressed driver, reads block and writes are shared, so the shim must also swallow GPIO writes —
  otherwise an output pin going off is read as a level on the over-current pins and rbp puts up "USB Error.
  Remove the device.".
- **Firmware version**: all rbp addresses are for v1.20; another version needs them re-found.
