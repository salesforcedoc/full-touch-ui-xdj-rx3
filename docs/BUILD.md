# Building

Minimal steps, tested once from a fresh clone (macOS + colima). Background: [`NOTES.md`](NOTES.md).

## You need

- Docker (on macOS: colima). Build in Docker **volumes** — the kernel needs a case-sensitive file system.
- An ARM musl cross-compiler on the host (`arm-linux-musleabihf-gcc`, e.g. Homebrew `musl-cross`) for the shims
  and dropbear
- Your own **XDJ-RX3 v1.20 firmware update**, unpacked so you have its `images/` folder (`rootfs.cramfs`,
  `pdj.tar.gz`, `gui.tar.gz`, `settings.tar.gz`)
- Pioneer DJ's **XDJ-RX3 GPL source**: its `linux-3.0.101` and `initramfs.tar.gz`
- dropbear 2024.86 source, mainline u-boot (git)
- A spare microSD card (1 GB or larger)

## 1. Kernel

In a Docker volume mounted at **`/work`** (the kernel config embeds `/work/initramfs`):

1. Put Pioneer's `linux-3.0.101` at `/work/linux-3.0.101` and apply the port:
   `git apply rx3-kernel/out/rx3-wandboard-port.patch`
2. Unpack Pioneer's `initramfs.tar.gz` into `/work` (gives `/work/initramfs`), then in `/work/initramfs`:
   `patch -p1 < rx3-kernel/initramfs-init.patch` (skips the ErP update check when `rx3.noerp` is set)
3. Build with the image from `rx3-kernel/Dockerfile` (gcc 4.6):
   ```sh
   docker build -t rx3-bsp rx3-kernel/
   docker run --rm --platform linux/amd64 -v <volume>:/work rx3-bsp sh -c 'cd /work/linux-3.0.101 &&
     make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- rx3_wandboard_defconfig &&
     make -j6 ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- uImage modules'
   ```
4. Copy `arch/arm/boot/uImage` → `rx3-kernel/out/uImage` and `fs/fuse/fuse.ko` → `rx3-kernel/out/modules/fuse.ko`.

## 2. u-boot

Mainline u-boot at commit `a44f46af`, in `debian:bookworm` with `gcc-arm-linux-gnueabihf make bison flex bc
libssl-dev libgnutls28-dev python3 python3-pyelftools python3-setuptools swig device-tree-compiler`:
```sh
cp rx3-uboot/uboot-rx3.config .config
make CROSS_COMPILE=arm-linux-gnueabihf- olddefconfig
make -j6 CROSS_COMPILE=arm-linux-gnueabihf-
```
Copy `SPL`, `u-boot.img` and `u-boot-dtb.img` into `rx3-uboot/`.

## 3. Firmware root file system

Extract `rootfs.cramfs` into a Docker volume (Debian package `util-linux-extra`):
```sh
fsck.cramfs --extract=/cram/rootfs /img/rootfs.cramfs
```

## 4. Shims

Copy the real files (not the symlinks: `cp -L`) of `libc.so.6 libdl.so.2 libpthread.so.0 libgcc_s.so.1` from the
extracted `rootfs/lib` into `rx3-rootfs/sysroot-lib/`, then:
```sh
rx3-rootfs/shims/build-shims.sh touchshim-rx3 knobshim-flx4 audioshim-rx3 uishim-rx3 hubtool
```

## 5. dropbear (ssh)

Unpack dropbear 2024.86 into `rx3-rootfs/src/dropbear-2024.86`, then:
```sh
./configure --host=arm-linux-musleabihf CC=arm-linux-musleabihf-gcc --disable-zlib --enable-static \
  --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline
make PROGRAMS="dropbear dropbearkey scp" MULTI=1 STATIC=1
```

## 6. SD image

Run the command in the header of `rx3-rootfs/build-rx3-sd.sh` (mount the extracted rootfs volume at `/cram`, the
firmware's `images/` at `/img`, the folder containing this repository at `/in`). Set `RX3_ROOT_PASSWORD`;
optionally `RX3_SSH_PUBKEY` (a public key file path inside the container). Output:
`rx3-out/wandboard-rx3native.img` (1 GB) — write it to the microSD with balenaEtcher or `dd`.

## 7. First boot

- MEGA4 hub with its own 5 V supply: port 1 = USB2 stick, port 2 = USB1 stick, port 3 = touch screen, port 4 =
  DDJ-FLX4 (own power supply). HDMI screen; Ethernet optional (the board doesn't answer ping; use ssh).
- The player starts by itself ~20 s after power-on.
- ssh: `root@<board address>`. Stop the player only with `kill -9 $(pidof rbp)` (SIGTERM reboots the system, as
  on the RX3); restart it with `/root/wand/rbp-restart.sh`.
