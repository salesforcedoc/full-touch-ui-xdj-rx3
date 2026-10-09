#!/bin/bash
# Route B: run the RX3 root file system as a chroot on a board that keeps its own kernel
# (see docs/PORTING.md). No GPU -- rbp's Vivante libraries need Pioneer's galcore -- so DirectFB
# runs the fbdev system with software rendering.
#
# Run on the target board as root. The chroot must already hold the RX3 rootfs plus /root/pdj,
# /root/gui and /root/settings (build-rx3-sd.sh shows that layout, or copy them from an RX3 SD).
# The shims must have been built for this rootfs (shims/build-shims.sh).
#
#   route-b.sh            set the chroot up if needed, then start edb_streamd and rbp -r
#   route-b.sh setup      the chroot, the shims and the USB rule only -- does not touch a running rbp
#   route-b.sh stop       stop rbp
#   route-b.sh status     what is running, plus the fb0 mode check
#   route-b.sh usb        USB sticks: the bridge, what is mounted, what rbp has open
#
# RX3_CHROOT overrides the chroot path (default /root/rx3/chroot). RX3_USB_RO=1 mounts sticks read-only
# (rbp then sees no rekordbox library), RX3_NO_USB_BRIDGE=1 skips the USB rule -- see docs/PORTING.md.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
C=${RX3_CHROOT:-/root/rx3/chroot}
[ -d "$C/root/pdj" ] || { echo "$C/root/pdj missing: is RX3_CHROOT right?" >&2; exit 1; }

fb_check() {
	# rbp lays out 1280x800; PORTING.md has the kernel command line for other screen sizes.
	# (Nothing here can set the mode: vc4's fbdev emulation rejects FBIOPUT_VSCREENINFO, so
	# DirectFB keeps whatever mode the kernel booted with and rbp draws into its top-left.)
	[ -e /dev/fb0 ] || { echo "  fb0: MISSING"; return; }
	echo "  fb0: $(cat /sys/class/graphics/fb0/virtual_size) x$(cat /sys/class/graphics/fb0/bits_per_pixel)bpp$(
		{ [ "$(cat /sys/class/graphics/fb0/virtual_size)" = 1280,800 ] &&
		  [ "$(cat /sys/class/graphics/fb0/bits_per_pixel)" = 16 ]; } || echo "  <- want 1280,800 x16: add video=... to the kernel command line")"
}

setup() {
	echo "=== [1] bind mounts ==="
	mkdir -p "$C/dev" "$C/proc" "$C/sys" "$C/tmp"
	mountpoint -q "$C/dev"  || mount --bind /dev  "$C/dev"
	mountpoint -q "$C/proc" || mount --bind /proc "$C/proc"
	mountpoint -q "$C/sys"  || mount --bind /sys  "$C/sys"
	mountpoint -q "$C/tmp"  || mount --bind /tmp  "$C/tmp"

	# The RX3's own /dev nodes are made by Pioneer's initramfs, which a chroot never runs, so make
	# them here. FIFOs matter: rbp lseeks them and reports "lseek: Illegal seek" either way, but it
	# opens the panel links through touchshim, and the USB queues through these. No /dev/paudiog0:
	# the RX3 has no such node and audioshim supplies the audio path.
	echo "=== [2] device nodes ==="
	for n in subucom_spi1.0 subucom_spi2.0 subucom_spi_rdy3.0 subucom_spi_rdy4.0 hidg0 gpiodrv; do
		[ -p "$C/dev/$n" ] || mkfifo -m 666 "$C/dev/$n"
	done
	for n in printkdrv0 tsc2007_2-0048; do
		[ -f "$C/dev/$n" ] || { : > "$C/dev/$n"; chmod 666 "$C/dev/$n"; }
	done

	# /dev/mem would be the board's own RAM here, not the i.MX6 registers rbp's GPU code expects:
	# park a regular empty file over it so the open succeeds but mmap cannot reach anything.
	# rbp still comes up without the MAP_SHARED/fd=-1 workaround PORTING.md mentions.
	echo "=== [3] block /dev/mem ==="
	: > /root/rx3-blocked-mem
	mountpoint -q "$C/dev/mem" || mount --bind /root/rx3-blocked-mem "$C/dev/mem"

	echo "=== [4] DirectFB software mode ==="
	mkdir -p "$C/usr/etc"
	cat > "$C/usr/etc/directfbrc" <<-'DFB'
		# Route B: the board's fb0, software rendering. Without this Pioneer's DirectFB picks its
		# Vivante gal gfxdriver, which needs Pioneer's galcore kernel driver.
		system=fbdev
		no-hardware
		disable-module=gal
		disable-module=linux_input
		no-linux-input-grab
	DFB

	echo "=== [5] shims + /tmp ==="
	for so in touchshim-rx3 knobshim-flx4 audioshim-rx3 uishim-rx3; do
		[ -f "$REPO/rx3-rootfs/shims/$so.so" ] || { echo "$so.so not built: run shims/build-shims.sh" >&2; exit 1; }
		install -m 755 "$REPO/rx3-rootfs/shims/$so.so" "$C/usr/lib/$so.so"
	done
	[ -f "$REPO/rx3-rootfs/shims/hubtool.so" ] &&
		install -m 755 "$REPO/rx3-rootfs/shims/hubtool.so" "$C/usr/lib/hubtool.so"
	echo "/usr/lib/touchshim-rx3.so /usr/lib/knobshim-flx4.so /usr/lib/audioshim-rx3.so /usr/lib/uishim-rx3.so" \
		> "$C/etc/ld.so.preload"
	# USB-stick notices. On the RX3 Pioneer's udev rules feed /proc/udev_*, which the kernel here
	# does not have, so nothing writes to them; they only have to exist for rbp to open.
	for n in usb1 usb2 usbctn1 usbctn2; do [ -p "$C/tmp/udev_$n" ] || mkfifo -m 666 "$C/tmp/udev_$n"; done
	rm -f "$C/tmp/guard_LocalDBServer" "$C/tmp/req_LocalDBServer"
	# uishim (the on-screen controls / touch calibration overlay) logs to /root/wand/uishim.log. A chroot
	# copied from an RX3 SD can lack that directory, and then every uishim log line -- taps on its buttons,
	# calibration targets, USB eject -- is silently dropped.
	mkdir -p "$C/root/wand"

	echo "=== [6] USB sticks -> rbp ==="
	# rbp opens /tmp/udev_usb1/2 and waits for "mount <dir>" / "umount <dir>" there (touchshim points its
	# open of Pioneer's /proc/udev_usb1/2 at those FIFOs). On the RX3 Pioneer's udev rules mount a stick at
	# /media/usb<slot>/<k> and post that message to a kernel queue; this kernel has no such queue, so the
	# board's own udev does both -- usb-mount.sh mounts the stick into the chroot and posts the message.
	# rx3-rootfs/usb-mount.sh has the details, including why the rule goes through systemd-run.
	for n in usb1 usb2 usbctn1 usbctn2; do [ -p "$C/tmp/udev_$n" ] || mkfifo -m 666 "$C/tmp/udev_$n"; done
	if [ -n "${RX3_NO_USB_BRIDGE:-}" ]; then
		echo "  skipped (RX3_NO_USB_BRIDGE)"
	else
		install -m 755 "$REPO/rx3-rootfs/usb-mount.sh" /usr/local/sbin/rx3-usb-mount.sh
		echo "RX3_CHROOT=$C" > /etc/default/rx3-usb
		if [ -n "${RX3_USB_RO:-}" ]; then echo "RX3_USB_RO=1" >> /etc/default/rx3-usb; fi
		cat > /etc/udev/rules.d/90-rx3-usb.rules <<-'RULE'
			# Route B: hand USB storage to the RX3 chroot's rbp. Pioneer's own rules write
			# mount/umount messages to /proc/udev_*, which this kernel has no queue for, and mount the
			# stick themselves. sd* only: the boot device, mmcblk and NVMe are left alone (usb-mount.sh
			# also refuses the device backing /).
			#
			# systemd-udevd runs RUN programs in its own slave mount namespace, where a mount never
			# reaches the host -- and so never reaches the chroot rbp reads the stick from. systemd-run
			# asks PID 1 to run the helper in the host namespace instead; --no-block keeps udev's
			# worker fast.
			ACTION=="add", SUBSYSTEM=="block", KERNEL=="sd*", RUN+="/usr/bin/systemd-run --no-block --collect --unit=rx3-usb-add-%k /usr/local/sbin/rx3-usb-mount.sh add %k"
			ACTION=="remove", SUBSYSTEM=="block", KERNEL=="sd*", RUN+="/usr/bin/systemd-run --no-block --collect --unit=rx3-usb-rem-%k /usr/local/sbin/rx3-usb-mount.sh remove %k"
		RULE
		rm -f /etc/udev/rules.d/90-rx3-usb.rules.disabled
		udevadm control --reload
		# a stick already inserted gets no "add" event from here on: ask for one
		udevadm trigger --action=add --subsystem-match=block --sysname-match='sd*'
		echo "  rule + helper installed; sticks already in are mounting"
	fi
}

start() {
	setup
	echo "=== [7] edb_streamd ==="
	if ! pgrep -f "[e]db_streamd" >/dev/null; then
		# EDB_BIN is what rbp's DeviceSQL client looks at. Detach the fds: this runs over ssh, and a
		# daemon still holding the session's stdout keeps the session open after the script exits.
		chroot "$C" /bin/sh -c 'export EDB_BIN=/usr/bin; exec taskset 0x1 /usr/bin/edb_streamd' \
			</dev/null >/tmp/edb.log 2>&1 &
		sleep 2
	fi
	echo "  edb_streamd: $(pgrep -f '[e]db_streamd' | wc -l) process"

	echo "=== [8] rbp -r ==="
	# Not /root/pdj/apl_start: it is Pioneer's board bring-up and would modprobe g_pmulti, mount the
	# UBIFS settings/gui partitions, reset the Wandboard's USB hub, and set the clock to 2021-01-01.
	# It also refuses to start rbp unless fw_printenv reports aplstart=on. -r is what it passes when
	# joglcd=rt (the RX3 SH2A jog LCD replaced by the i.MX RT).
	cd "$C/root/pdj"
	nohup chroot "$C" /bin/sh -c 'cd /root/pdj && exec ./rbp -r' </dev/null >/tmp/rbp.log 2>&1 &
	sleep 5
	pgrep -f "[.]/rbp" >/dev/null && echo "  rbp: running (log /tmp/rbp.log)" || {
		echo "  rbp: exited -- /tmp/rbp.log:"; sed 's/^/    /' /tmp/rbp.log; exit 1; }
	fb_check
	echo "  shim logs: /tmp/*shim*.log"
}

usb_status() {
	for f in /usr/local/sbin/rx3-usb-mount.sh /etc/udev/rules.d/90-rx3-usb.rules /etc/default/rx3-usb; do
		[ -e "$f" ] && echo "$(printf '%-42s' "$f") present" || echo "$(printf '%-42s' "$f") MISSING"
	done
	echo "sticks:"
	if grep -q " $C/media/usb[12]/" /proc/mounts; then
		grep -E " $C/media/usb[12]/" /proc/mounts | awk '{ printf "  %s -> %s (%s)\n", $1, $2, $4 }'
	else
		echo "  none mounted"
	fi
	# rbp holds the queues open itself; systemd-run's transient units are the bridge's own runs
	echo "udev FIFOs held by: $(fuser /tmp/udev_usb1 /tmp/udev_usb2 2>/dev/null | tr -s ' ' ' ' | sed 's/^ //')"
	P=$(pgrep -f "[.]/rbp" | head -1)
	if [ -n "$P" ]; then
		echo "rbp $P: DET_USB1=$(dd if=/proc/$P/mem bs=4 skip=$((0x03256888 / 4)) count=1 2>/dev/null | od -An -tu4 | tr -d ' ')"
	fi
	[ -f /tmp/usb-mount.log ] && { echo "--- /tmp/usb-mount.log"; tail -5 /tmp/usb-mount.log | sed 's/^/  /'; }
}

case "${1:-start}" in
start) start ;;
setup) setup ;;
stop)  # SIGKILL on purpose: rbp handles SIGTERM by rebooting the board (see docs/BUILD.md).
	pkill -9 -f "[.]/rbp" && echo "rbp stopped" || echo "rbp was not running" ;;
usb)
	case "${2:-}" in
	rescan)	udevadm trigger --action=add --subsystem-match=block --sysname-match='sd*'
		echo "asked udev for an 'add' on every sd* device" ;;
	*) usb_status ;;
	esac ;;
status)
	pgrep -f "[.]/rbp" >/dev/null && echo "rbp: running ($(pgrep -f '[.]/rbp' | wc -l))" || echo "rbp: not running"
	echo "edb_streamd: $(pgrep -f '[e]db_streamd' | wc -l) process"
	fb_check
	echo "usb sticks: $(grep -cE " $C/media/usb[12]/" /proc/mounts) mounted  (route-b.sh usb)"
	for f in /tmp/touchshim*.log /tmp/knobshim*.log /tmp/audioshim*.log /tmp/uishim*.log; do
		[ -f "$f" ] && { echo "--- $f"; tail -3 "$f" | sed 's/^/  /'; }
	done ;;
*) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//' ; exit 2 ;;
esac
