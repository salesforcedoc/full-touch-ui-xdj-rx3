# Building

A minimal outline — not yet tested from scratch, so expect to adapt the steps. Background and the reasons behind
each step: [`NOTES.md`](NOTES.md).

## You need

- Linux or macOS with Docker (on macOS: colima, with a case-sensitive Docker volume for the kernel)
- An ARM musl cross-compiler (`arm-linux-musleabihf-gcc`, e.g. from musl-cross) — for the shims and dropbear
- Your own **XDJ-RX3 v1.20 firmware update**, unpacked so you have its `images/` folder:
  `rootfs.cramfs`, `pdj.tar.gz`, `gui.tar.gz`, `settings.tar.gz`
- Pioneer DJ's **XDJ-RX3 GPL source** package: `linux-3.0.101` and the initramfs
- A microSD card (2 GB or larger)

## 1. Kernel

```sh
docker build -t rx3-bsp rx3-kernel/
```
In a Docker volume mounted at `/work`: copy Pioneer's `linux-3.0.101` there, apply
`rx3-kernel/out/rx3-wandboard-port.patch` (`git apply`), and unpack Pioneer's initramfs to `/work/initramfs`.
Make one change to the initramfs `init`: skip the ErP check (`gpio -i 3 30`) when `rx3.noerp` is on the kernel
command line (the pin is unconnected on the Wandboard and can trigger the USB-update mode). Then:
```sh
docker run --rm --platform linux/amd64 -v <volume>:/work rx3-bsp sh -c \
  'cd /work/linux-3.0.101 && make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- rx3_wandboard_defconfig &&
   make -j6 ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- uImage modules'
```
Copy `arch/arm/boot/uImage` to `rx3-kernel/out/uImage` and `fs/fuse/fuse.ko` to `rx3-kernel/out/modules/fuse.ko`.

## 2. u-boot

Mainline u-boot at commit `a44f46af`, with `rx3-uboot/uboot-rx3.config` as `.config`; `make` it for ARM and copy
`SPL`, `u-boot.img` and `u-boot-dtb.img` into `rx3-uboot/`.

## 3. Shims

Copy `libc.so.6`, `libdl.so.2`, `libpthread.so.0` and `libgcc_s.so.1` from the firmware's root file system
(`/lib`) into `rx3-rootfs/sysroot-lib/`, then:
```sh
rx3-rootfs/shims/build-shims.sh touchshim-rx3 knobshim-flx4 audioshim-rx3 uishim-rx3 hubtool
```

## 4. dropbear (ssh)

Build dropbear 2024.86 as one static binary with the musl cross-compiler and place it at
`rx3-rootfs/src/dropbear-2024.86/dropbearmulti` (`MULTI=1 STATIC=1`, programs `dropbear dropbearkey scp`).

## 5. SD image

Extract `rootfs.cramfs` (`fsck.cramfs --extract`) into a Docker volume as `rootfs/`, then run the command in the
header of `rx3-rootfs/build-rx3-sd.sh` (set `RX3_ROOT_PASSWORD`; optionally `RX3_SSH_PUBKEY`). It writes
`rx3-out/wandboard-rx3native.img`. Adjust the mount paths in that command to where your files are.

Write the image to the microSD card (e.g. `dd` or balenaEtcher).

## 6. First boot

- Hardware: MEGA4 hub with its own 5 V supply — port 1 = USB2 stick, port 2 = USB1 stick, port 3 = touch screen,
  port 4 = DDJ-FLX4 (own power supply). HDMI screen, Ethernet optional.
- The player starts by itself about 20 s after power-on.
- ssh (optional): `root@<board address>` with your key or password. `tools/rx3ssh` needs `RX3_HOST` set.
- Stop the player only with `kill -9 $(pidof rbp)` (SIGTERM reboots the system, as on the RX3); restart it with
  `/root/wand/rbp-restart.sh`.
