#!/bin/bash
# Build RX3-native shims against the RX3 v1.20 rootfs glibc 2.13 and verify every import resolves there.
set -e
cd "$(dirname "$0")"
LIB=../sysroot-lib
CC="arm-linux-musleabihf-gcc -O2 -march=armv5t -mfloat-abi=soft -fno-stack-protector -fPIC -shared -nostdlib"
LIBS="-L$LIB -Wl,-l:libc.so.6 -Wl,-l:libdl.so.2 -Wl,-l:libpthread.so.0 -Wl,-l:libgcc_s.so.1 -Wl,-rpath-link,$LIB"
for n in ${@:-touchshim-rx3}; do
  $CC -Wall -o $n.so $n.c $LIBS
  for s in $(arm-linux-musleabihf-objdump -T $n.so | awk '/\*UND\*/ && !/ w /{print $NF}'); do
    f=""
    for l in libc.so.6 libdl.so.2 libpthread.so.0 libgcc_s.so.1; do
      arm-linux-musleabihf-objdump -T $LIB/$l | awk '!/\*UND\*/{print $NF}' | grep -qx "$s" && f=1 && break
    done
    [ -z "$f" ] && { echo "MISSING $s in $n"; exit 1; }
  done
  echo "$n.so ok ($(arm-linux-musleabihf-objdump -T $n.so | grep -c UND) imports)"
done
