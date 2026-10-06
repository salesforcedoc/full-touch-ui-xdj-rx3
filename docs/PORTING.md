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

**B. Your board's own kernel + chroot (any ARM board, untested)** — no GPU (rbp's Vivante libraries need
Pioneer's galcore). In the RX3 root file system as a chroot:

1. Bind-mount `/dev /proc /sys /tmp`.
2. FIFOs (not regular files) for `/dev/subucom_spi1.0 subucom_spi2.0 subucom_spi_rdy3.0 subucom_spi_rdy4.0 hidg0
   gpiodrv`; regular files for `/dev/printkdrv0 tsc2007_2-0048`; no `/dev/paudiog0`.
3. Block `/dev/mem` (it would poke i.MX6 registers) and turn rbp's resulting `mmap(MAP_SHARED, fd=-1)` into an
   anonymous private mapping in a shim.
4. DirectFB software mode: remove the `gal` gfxdriver and the `linux_input` driver; `system=fbdev`,
   `no-hardware`. Stub `libg2d` (album-art scaling only).
5. `/tmp/udev_usb1/2`, `udev_usbctn1/2` FIFOs fed by udev (`mount <dir>` / `umount <dir>`).
6. `localhost` must resolve; remove stale `/tmp/guard_LocalDBServer /tmp/req_LocalDBServer` before starting.
7. Start `edb_streamd`, then `rbp -r` from `/root/pdj`; shims via the chroot's `/etc/ld.so.preload`.

## Per device

- **Touch screen**: touchshim auto-detects evdev; set `TSC_RAW_*` (range) and `TSC_OFF_X/Y` (offset). Calibrate with
  `touch /tmp/uishim.cal` (9 targets, taps logged with `UISHIM_TOUCH_LOG=1`).
- **Controller**: audioshim expects `hw:DDJFLX4,0`, 4 ch (master 1/2, headphones 3/4); knobshim's `build_maps()`
  maps FLX4 MIDI to RX3 keys, plus the FLX4's keep-alive, jog resolution and LEDs. Key codes: the ones used
  are `#define`s in `knobshim-flx4.c` / `uishim-rx3.c`; rbp v1.20 lists all of them (with names) in
  `ui::KeyInput::keyCodeAsText()`. Send a key with knobshim's inject file: `printf '0x0202 9 1\n' > /tmp/flx4-inject`.
- **Board signals**: the over-current GPIO fix and the cpuinfo board revision (see `NOTES.md`) are needed on any
  board; with no `/dev/gpiodrv` the GPIO fix simply isn't used.
- **Firmware version**: all rbp addresses are for v1.20; another version needs them re-found.
