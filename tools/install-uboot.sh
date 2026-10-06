#!/bin/bash
# Install rx3-uboot/{SPL,u-boot.img,u-boot-dtb.img} onto the running RX3-native card over ssh.
set -e
cd "$(dirname "$0")/.."
until tools/rx3ssh true 2>/dev/null; do sleep 2; done
tools/rx3ssh 'cat > /root/wand/SPL' < rx3-uboot/SPL
tools/rx3ssh 'cat > /root/wand/u-boot-dtb.img' < rx3-uboot/u-boot-dtb.img
tools/rx3ssh 'set -e; cd /root/wand
  dd if=SPL of=/dev/mmcblk0 bs=1024 seek=1 conv=fsync 2>/dev/null
  dd if=u-boot-dtb.img of=/dev/mmcblk0 bs=512 seek=138 conv=fsync 2>/dev/null
  cp u-boot-dtb.img /u-boot-dtb.img; sync
  echo "SPL  card: $(dd if=/dev/mmcblk0 bs=1024 skip=1 count=47 2>/dev/null | md5sum | cut -c1-32)  file: $(md5sum < SPL | cut -c1-32)"
  echo "uboot card: $(dd if=/dev/mmcblk0 bs=512 skip=138 count=$(( ($(wc -c < u-boot-dtb.img)+511)/512 )) 2>/dev/null | head -c $(wc -c < u-boot-dtb.img) | md5sum | cut -c1-32)  file: $(md5sum < u-boot-dtb.img | cut -c1-32)"
  echo "fs copy:    $(md5sum < /u-boot-dtb.img | cut -c1-32)"'
