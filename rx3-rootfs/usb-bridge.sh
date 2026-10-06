#!/bin/sh
# usb-bridge.sh usb1|usb2 -- forward Pioneer's /proc/udev_<slot> stick messages to rbp's /tmp/udev_<slot> FIFO,
# debounced (started by init.d/wandboard; replaces a plain "cat").
#
# Pioneer's /proc/udev_* queue (fs/proc/udev_usb.c) hands out exactly one "mount <dir>" / "umount <dir>"
# message per read() and blocks while empty, so "dd bs=512 count=1" reads one message.
#
# Why: pulling a stick and plugging another in within a second or two left rbp's USB1 handling
# stuck -- every later mount message was ignored until rbp restarted. The kernel log showed a stick that
# appeared and vanished at once, so rbp was most likely told about a stick that was already gone. Now:
#   - "mount <dir>" is held until <dir> has stayed mounted for SETTLE_TENTHS (2.5 s); gone by then = dropped.
#   - before a mount for a new dir, a "umount" is sent for any dir still announced (rbp never sees two sticks).
#   - "umount <dir>" is only forwarded for the dir that was announced (its matching mount was not dropped).
# Log: /tmp/usb-bridge-<slot>.log
n=$1
q=/proc/udev_$n
f=/tmp/udev_$n
log=/tmp/usb-bridge-$n.log
SETTLE_TENTHS=25
ann=""

say() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $log; }
send() { echo -n "$1" > $f; say "-> $1"; }
mounted() { grep -q " $1 " /proc/mounts; }

[ -p $f ] || mkfifo -m 666 $f
# a stick already mounted (bridge restarted) was announced by the previous bridge
ann=$(grep -o " /media/$n/[^ ]*" /proc/mounts | head -1 | tr -d ' ')
say "bridge start (announced: ${ann:-none})"
while :; do
	msg=$(dd if=$q bs=512 count=1 2>/dev/null)
	[ -n "$msg" ] || { sleep 1; continue; }
	say "<- $msg"
	set -- $msg
	case "$1" in
	mount)
		t=0
		while [ $t -lt $SETTLE_TENTHS ] && mounted "$2"; do usleep 500000; t=$((t + 5)); done
		if [ $t -lt $SETTLE_TENTHS ]; then say "drop: $2 gone within settle time"; continue; fi
		[ -n "$ann" ] && [ "$ann" != "$2" ] && send "umount $ann"
		send "$msg"
		ann=$2
		;;
	umount)
		if [ "$2" = "$ann" ]; then send "$msg"; ann=""; else say "drop: $2 was not announced"; fi
		;;
	*)
		send "$msg"
		;;
	esac
done
