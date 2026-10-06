#!/bin/bash
# Build an RX3-native SD image for the Wandboard Quad (Rev D1).
# Runs inside debian:bookworm. Mounts:
#   /cram   rx3root volume (fsck.cramfs --extract of v1.20 rootfs.cramfs, at /cram/rootfs)
#   /img    ~/Downloads/rx3_v120/iso_root/images   (Pioneer v1.20 update payload)
#   /in     the folder that contains this repository (and the ssh public key, if any)
#   /out    output dir
# Settings: RX3_ROOT_PASSWORD (required), RX3_SSH_PUBKEY (optional). Example:
#   docker run --rm -e RX3_ROOT_PASSWORD='choose-one' -v rx3root:/cram -v ~/Downloads/rx3_v120/iso_root/images:/img:ro \
#     -v ~/Downloads:/in -v ~/Downloads/wandboard-build/rx3-out:/out debian:bookworm \
#     bash /in/<this repository>/rx3-rootfs/build-rx3-sd.sh
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)    # this repository (any folder name), as mounted in the container
apt-get update -qq >/dev/null
apt-get install -y -qq e2fsprogs fdisk u-boot-tools openssl >/dev/null 2>&1

IMG=/out/wandboard-rx3native.img
SIZE_MB=1024
PART_START_MB=16                     # Pioneer bootcmd_mmc reads the raw kernel at 1 MB..9 MB
R=/tmp/rootfs
rm -rf $R; mkdir -p $R

echo "=== [1] RX3 rootfs (rootfs.cramfs, as installed by Pioneer) ==="
cp -a /cram/rootfs/. $R/

echo "=== [2] /root/pdj, /root/gui, /root/settings (Pioneer SD-boot layout) ==="
mkdir -p $R/root/gui $R/root/settings
tar -xzf /img/pdj.tar.gz -C $R/root
tar -xzf /img/gui.tar.gz -C $R/root/gui
tar -xzf /img/settings.tar.gz -C $R/root/settings
# rbp: Pioneer's own v1.20 binary from pdj.tar.gz, unmodified (md5 4f2efcfc0c9e3f539289f863acfddcc6); the shims
# supply what the RX3 hardware would (touchshim-rx3.c lists the details). RX3_RBP = path inside the container of
# another rbp to use instead (Pioneer's is kept as rbp.pioneer); the shims expect v1.20 addresses.
if [ -n "${RX3_RBP:-}" ]; then
	mv $R/root/pdj/rbp $R/root/pdj/rbp.pioneer
	cp "$RX3_RBP" $R/root/pdj/rbp; chmod 755 $R/root/pdj/rbp
fi
md5sum $R/root/pdj/rbp*

echo "=== [3] Wandboard additions: ssh, dhcp, credentials ==="
install -m 755 $REPO/rx3-rootfs/src/dropbear-2024.86/dropbearmulti $R/usr/sbin/dropbearmulti
for p in dropbear dropbearkey scp; do ln -sf dropbearmulti $R/usr/sbin/$p; done
install -m 755 $REPO/rx3-rootfs/wandboard.initd $R/etc/rc.d/init.d/wandboard
sed -i 's/^cfg_services="\(.*\)"/cfg_services="\1 wandboard"/; s/^cfg_services_r="/cfg_services_r="wandboard /' $R/etc/rc.d/rc.conf
grep '^cfg_services' $R/etc/rc.d/rc.conf
# root password (console + ssh): RX3_ROOT_PASSWORD, passed into the container (docker run -e RX3_ROOT_PASSWORD=...)
: "${RX3_ROOT_PASSWORD:?set RX3_ROOT_PASSWORD to the root password for the board}"
HASH=$(openssl passwd -1 "$RX3_ROOT_PASSWORD")
sed -i "s|^root:[^:]*:|root:${HASH}:|" $R/etc/shadow
# ssh public key for root: RX3_SSH_PUBKEY (path inside the container; default the key file kept next to this project)
PUB=${RX3_SSH_PUBKEY:-/in/rx3_board_ed25519.pub}
mkdir -p $R/root/.ssh; chmod 700 $R/root/.ssh
if [ -f "$PUB" ]; then cp "$PUB" $R/root/.ssh/authorized_keys; chmod 600 $R/root/.ssh/authorized_keys
else echo "no ssh key ($PUB): password login only"; fi
grep -q /usr/sbin/dropbear $R/etc/shells 2>/dev/null || true
echo wandboard-rx3 > $R/etc/hostname
# touch: USB-HID panel -> rbp tsc2007 (shim is inert outside rbp); keep DirectFB off the evdev
for so in touchshim-rx3 knobshim-flx4 audioshim-rx3 uishim-rx3; do install -m 755 $REPO/rx3-rootfs/shims/$so.so $R/usr/lib/$so.so; done
install -m 755 $REPO/rx3-rootfs/shims/hubtool.so $R/usr/lib/hubtool.so   # FLX4 port power-cycle at boot (not preloaded)
mkdir -p $R/root/wand
install -m 755 $REPO/rx3-rootfs/usb-bridge.sh $REPO/rx3-rootfs/rbp-restart.sh $R/root/wand/
# exFAT sticks: Pioneer mounts them with exfat-fuse; its fuse.ko is built for Pioneer's kernel, so install ours
# (built against our kernel tree: rx3-kernel/out/modules/fuse.ko, vermagic 3.0.101-2790-gc248ed7-gc4dcc22-dirty).
# Pioneer's init.d/fuse modprobes it at boot.
KREL=$(grep -a -o 'vermagic=[^ ]*' $REPO/rx3-kernel/out/modules/fuse.ko | head -1 | cut -d= -f2)   # = the kernel's release
echo "kernel release (from fuse.ko): $KREL"
mkdir -p $R/lib/modules/$KREL/kernel/fs/fuse
install -m 644 $REPO/rx3-kernel/out/modules/fuse.ko $R/lib/modules/$KREL/kernel/fs/fuse/fuse.ko
grep -q fuse.ko $R/lib/modules/$KREL/modules.dep 2>/dev/null || echo "kernel/fs/fuse/fuse.ko:" >> $R/lib/modules/$KREL/modules.dep
echo "/usr/lib/touchshim-rx3.so /usr/lib/knobshim-flx4.so /usr/lib/audioshim-rx3.so /usr/lib/uishim-rx3.so" > $R/etc/ld.so.preload
mkdir -p $R/usr/etc; install -m 644 $REPO/rx3-rootfs/directfbrc $R/usr/etc/directfbrc

echo "=== [4] boot files ==="
mkdir -p $R/boot
cp $REPO/rx3-kernel/out/uImage $R/boot/uImage
cp $REPO/rx3-uboot/u-boot-dtb.img $R/u-boot-dtb.img      # SPL FS-mode payload
cp $REPO/rx3-rootfs/fw_env.wandboard.txt $R/boot/

echo "=== [5] ext4 (features a 3.0 kernel can mount) ==="
PART_MB=$((SIZE_MB - PART_START_MB))
rm -f /tmp/rootfs.ext4
mkfs.ext4 -q -F -L rx3root -O ^metadata_csum,^metadata_csum_seed,^64bit,^orphan_file \
	-d $R /tmp/rootfs.ext4 ${PART_MB}M

echo "=== [6] assemble card image ==="
rm -f $IMG
dd if=/dev/zero of=$IMG bs=1M count=$SIZE_MB status=none
echo "${PART_START_MB}M,,L" | sfdisk -q $IMG
dd if=$REPO/rx3-uboot/SPL        of=$IMG bs=1K seek=1   conv=notrunc status=none
dd if=$REPO/rx3-uboot/u-boot.img of=$IMG bs=512 seek=138 conv=notrunc status=none   # 0x8A
mkenvimage -s 0x2000 -o /tmp/env.bin $REPO/rx3-rootfs/fw_env.wandboard.txt
dd if=/tmp/env.bin of=$IMG bs=1K seek=768 conv=notrunc status=none                                  # 0xC0000
dd if=$REPO/rx3-kernel/out/uImage of=$IMG bs=512 seek=2048 conv=notrunc status=none   # 0x800
dd if=/tmp/rootfs.ext4 of=$IMG bs=1M seek=$PART_START_MB conv=notrunc status=none
sfdisk -l $IMG
ls -la $IMG
