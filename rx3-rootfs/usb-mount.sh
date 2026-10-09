#!/bin/sh
# usb-mount.sh add|remove <kernelname> -- hand a USB stick to the RX3 rootfs chroot's rbp (Route B).
#
# On the RX3 Pioneer's 12-usb-memory-auto-mount.rules mounts the partition at /media/usb<slot>/<k> and
# posts "mount <dir>" to the kernel queue /proc/udev_<slot>, which rbp reads. A board running its own
# kernel has no /proc/udev_*, so this does the same job from the board's own udev: mount the stick into
# the chroot, then post the same message to the /tmp FIFO that touchshim points rbp's open at.
#
# It is the Route B counterpart of usb-bridge.sh, which stays for boards with a Pioneer kernel (there
# the kernel queue exists and there is nothing to mount).
#
# Called from /etc/udev/rules.d/90-rx3-usb.rules, through systemd-run: systemd-udevd keeps its own
# (slave) mount namespace, so a mount made by a udev RUN program never reaches the host or the chroot.
#
#   usb-mount.sh add sda1        mount /dev/sda1 into the chroot and tell rbp
#   usb-mount.sh remove sda1     unmount it and tell rbp
#
# Config: /etc/default/rx3-usb   RX3_CHROOT=...   (default /root/rx3/chroot)
#                                RX3_USB_RO=1     mount read-only (default: read-write, as the RX3 does)
# Log:    /tmp/usb-mount.log (same tmpfs as the chroot's /tmp)
#
# Always exits 0 -- udev RUN programs must not fail the event.

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH

action=${1:-}
k=${2:-}
[ -n "$action" ] && [ -n "$k" ] || { echo "usage: $0 add|remove <kernelname>" >&2; exit 2; }

C=${RX3_CHROOT:-/root/rx3/chroot}
[ -r /etc/default/rx3-usb ] && . /etc/default/rx3-usb
dev=/dev/$k
log=/tmp/usb-mount.log

say() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> "$log" 2>/dev/null; }

# One message, no trailing newline: exactly what Pioneer's rule writes to /proc/udev_<slot> (echo -n).
# Opened O_RDWR so this can never block -- rbp holds the FIFO open itself, and if it is not running yet
# the message waits in the pipe for it, as the kernel queue would.
notify() {
	[ -p "/tmp/udev_$1" ] || mkfifo -m 666 "/tmp/udev_$1" 2>/dev/null
	( exec 3<>"/tmp/udev_$1" && printf %s "$2" >&3 ) 2>/dev/null
	say "-> $2"
}

# Read-WRITE by default, the way Pioneer's own rule mounts a stick (rw,flush,dmask/fmask=000,...). It is
# not a matter of taste: rbp's DeviceSQL opens PIONEER/rekordbox/export.pdb (and exportExt.pdb) for
# writing, so on a read-only mount that open fails and rbp quietly falls back to browsing the raw
# filesystem -- the stick still appears in SOURCE with its capacity, but with no playlists, tracks or
# artwork on it. RX3_USB_RO=1 mounts read-only anyway, for a data-only stick.
if [ -n "${RX3_USB_RO:-}" ]; then mopt=ro; else mopt=rw; fi
# vfat flushes on close, so an unannounced yank does not lose writes (Pioneer passes this too).
if [ "$mopt" = rw ]; then vopt=",flush"; else vopt=; fi

# The mount already in /proc/mounts for this device, if any ("" = not mounted).
mounted_at() { awk -v d="$dev" '$1 == d { print $2; exit }' /proc/mounts; }

# rw or ro, the way this device is currently mounted ("" = not mounted).
mounted_mode() {
	awk -v d="$dev" '$1 == d {
		n = split($4, o, ",")
		for (i = 1; i <= n; i++) if (o[i] == "rw" || o[i] == "ro") { print o[i]; exit }
	}' /proc/mounts
}

case "$action" in
add)
	[ -b "$dev" ] || { say "add $k: no such block device"; exit 0; }

	# never mount the disk the board booted from (a board whose root is on sd*)
	root=$(findmnt -n -o SOURCE / 2>/dev/null)
	[ -n "$root" ] && [ "$dev" = "$root" ] && { say "add $k: is the root device"; exit 0; }

	cur=$(mounted_at)
	for s in usb1 usb2; do
		[ "$cur" = "$C/media/$s/$k" ] || continue
		have=$(mounted_mode)
		# already where we would put it, the way we would mount it (a repeat event): just re-announce
		# it, which is also what rbp needs if it started after the stick was mounted
		if [ "$have" = "$mopt" ]; then
			say "add $k: already at media/$s/$k $mopt, re-announcing"
			notify "$s" "mount /media/$s/$k"
			exit 0
		fi
		# mounted the other way -- a hand mount, or left over from an earlier config. rbp could not
		# read a library from it, so redo the mount and let rbp scan it again.
		say "add $k: at media/$s/$k but $have, want $mopt -- remounting"
		notify "$s" "umount /media/$s/$k"
		if umount "$cur" 2>>"$log"; then
			cur=
		else
			# busy: rbp is using it, so it is read the way it needs to be -- leave it alone
			say "add $k: umount of $cur failed, leaving it mounted"
			notify "$s" "mount /media/$s/$k"
			exit 0
		fi
	done
	# mounted somewhere else (a desktop automounter, a hand mount): someone else owns it
	[ -n "$cur" ] && { say "add $k: already mounted at $cur"; exit 0; }

	# exfat: the host has no mount.exfat-fuse, so use the kernel module the chroot shares
	case "$(/sbin/blkid -s TYPE -o value "$dev" 2>/dev/null)" in
	vfat|msdos) type=vfat;    opts="$mopt$vopt,noatime,shortname=mixed,utf8" ;;
	exfat)      type=exfat;   opts="$mopt,noatime"; modprobe exfat 2>/dev/null ;;
	# hfsplus sticks would need fsck.hfsplus, which is not on a stock board (README: HFS+ unsupported)
	hfsplus)    say "add $k: hfsplus, unsupported here"; exit 0 ;;
	*)          exit 0 ;;   # no filesystem of its own: the whole-disk event of a partitioned stick
	esac

	# USB1 first, USB2 if USB1 is taken (rbp handles one stick at a time per slot)
	slot=
	for s in usb1 usb2; do
		grep -q " $C/media/$s/" /proc/mounts || { slot=$s; break; }
	done
	[ -n "$slot" ] || { say "add $k: usb1 and usb2 are both taken"; exit 0; }

	mp=$C/media/$slot/$k
	mkdir -p "$mp" || { say "add $k: cannot create $mp"; exit 0; }
	if ! mount -t "$type" -o "$opts" "$dev" "$mp" 2>>"$log"; then
		say "add $k: mount of $dev at $mp failed"
		rmdir "$mp" 2>/dev/null
		exit 0
	fi
	# settle: a stick pulled while it was being mounted is not one to tell rbp about
	i=0
	while [ $i -lt 5 ] && [ -b "$dev" ] && mountpoint -q "$mp"; do sleep 0.2; i=$((i + 1)); done
	mountpoint -q "$mp" || { say "add $k: gone before it settled"; exit 0; }
	notify "$slot" "mount /media/$slot/$k"
	;;

remove)
	# rbp's own USB STOP (uishim's EJECT button) unmounts the media itself, so finding nothing mounted
	# here is normal, not an error
	for s in usb1 usb2; do
		mp=$C/media/$s/$k
		grep -q " $mp " /proc/mounts || continue
		umount -l "$mp" 2>>"$log"
		rmdir "$mp" 2>/dev/null
		notify "$s" "umount /media/$s/$k"
		exit 0
	done
	say "remove $k: was not mounted"
	;;

*)	echo "usage: $0 add|remove <kernelname>" >&2; exit 2 ;;
esac

exit 0
