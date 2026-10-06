#!/bin/bash
# Build the RX3 kernel in the rx3work volume, install uImage (raw @0x800 + /boot) and galcore.ko on the board, reboot.
set -e; cd "$(dirname "$0")/.."
TAG=${1:-dev}
docker run --rm --platform linux/amd64 -v rx3work:/work rx3-bsp sh -c 'cd /work/linux-3.0.101 && make -j6 ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- uImage modules > /work/wand.log 2>&1 || { grep -E "error" /work/wand.log | head; exit 1; }; cp arch/arm/boot/uImage /work/out/uImage-'$TAG'; cp drivers/mxc/gpu-viv/galcore.ko /work/out/galcore-'$TAG'.ko'
for f in uImage-$TAG galcore-$TAG.ko; do docker run --rm --platform linux/amd64 -v rx3work:/work rx3-bsp cat /work/out/$f > rx3-kernel/out/$f; done
tools/rx3ssh 'cat > /root/wand/uImage' < rx3-kernel/out/uImage-$TAG
tools/rx3ssh 'cat > /root/wand/galcore.ko' < rx3-kernel/out/galcore-$TAG.ko
L=$(md5 -q rx3-kernel/out/uImage-$TAG)
R=$(tools/rx3ssh 'dd if=/root/wand/uImage of=/dev/mmcblk0 bs=512 seek=2048 conv=fsync 2>/dev/null; sync; dd if=/dev/mmcblk0 bs=512 skip=2048 count=$(( ($(wc -c < /root/wand/uImage)+511)/512 )) 2>/dev/null | head -c $(wc -c < /root/wand/uImage) | md5sum | cut -c1-32')
[ "$L" = "$R" ] || { echo "VERIFY FAILED local=$L card=$R"; exit 1; }
tools/rx3ssh 'cp /root/wand/uImage /boot/uImage; sync; (sleep 1; reboot) >/dev/null 2>&1 &'
echo "installed uImage-$TAG ($L), rebooting"
sleep 20; until tools/rx3ssh true 2>/dev/null; do sleep 3; done; tools/rx3ssh 'uname -v'
