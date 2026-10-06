#!/bin/sh
# rbp-restart.sh -- restart rbp the way Pioneer's apl_start does, then bring the USB sticks back.
# Run over ssh, or from uishim's "USB1 STUCK - HOLD TO RESTART" button (SOURCE screen).
# Log: /root/wand/rbp-restart.log; rbp output: /root/wand/rbp.log
exec </dev/null >/root/wand/rbp-restart.log 2>&1
trap '' HUP
say() { echo "$(cut -d' ' -f1 /proc/uptime) $*"; }
det_usb1() { P=$(pidof rbp) && dd if=/proc/$P/mem bs=4 skip=$((0x03256888 / 4)) count=1 2>/dev/null | hexdump -e '1/4 "%d"'; }

say "restart"
rm -f /tmp/knobshim.usb1stuck                   # knobshim test switch for the STUCK button: one-shot
P=$(pidof rbp) && kill -9 $P
for i in 1 2 3 4 5 6 7 8 9 10; do pidof rbp >/dev/null || break; sleep 1; done
cd /root/pdj
(EDB_BIN=/usr/bin FLX_VERBOSE=1 exec /root/pdj/rbp -r >/root/wand/rbp.log 2>&1) &
say "rbp started"

# Re-announce the mounted sticks once rbp is up (20 s is enough). knobshim's heal also re-announces a
# rekordbox stick in USB1 until it registers; a stick without a library has only this. Select USB1 once registered.
sleep 20
for n in usb1 usb2; do
	for d in $(grep -o " /media/$n/[^ ]*" /proc/mounts); do echo -n "mount $d" > /tmp/udev_$n; say "announced $d"; done
done
if grep -q ' /media/usb1/' /proc/mounts; then
	for i in $(seq 1 45); do [ "$(det_usb1)" = 2 ] && break; sleep 2; done
	if [ "$(det_usb1)" = 2 ]; then printf '0x0209 9 1\n' > /tmp/flx4-inject; say "USB1 registered, selected"
	else say "USB1 still not registered"; fi
fi
say "done"
