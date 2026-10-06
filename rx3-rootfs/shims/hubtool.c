/*
 * hubtool.c — inspect / power-cycle a USB hub port from userspace (usbdevfs control transfers).
 * Built as a .so (no glibc-2.13 crt files for executables); runs from its constructor:
 *   HUBTOOL=status  HUB=/dev/bus/usb/002/002 LD_PRELOAD=/root/wand/hubtool.so /bin/true
 *   HUBTOOL=cycle   HUB=... PORT=3 [OFF_MS=1500] LD_PRELOAD=... /bin/true
 * status: hub descriptor (ports, wHubCharacteristics power-switching mode) + every port's status.
 * cycle:  ClearPortFeature(PORT_POWER), wait, SetPortFeature(PORT_POWER) -> like a replug.
 * reset:  USB reset of the hub itself (USBDEVFS_RESET); it re-enumerates with all its ports.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

struct ctrl { uint8_t bRequestType, bRequest; uint16_t wValue, wIndex, wLength; uint32_t timeout; void *data; };
#define USBDEVFS_CONTROL _IOWR('U', 0, struct ctrl)
#define USBDEVFS_RESET   _IO('U', 20)

#define say(...) do { char b_[256]; int n_ = snprintf(b_, sizeof b_, __VA_ARGS__); write(2, b_, n_); } while (0)

static int ctl(int fd, int type, int req, int val, int idx, void *buf, int len)
{
    struct ctrl c = { type, req, val, idx, len, 1000, buf };
    return ioctl(fd, USBDEVFS_CONTROL, &c);
}

__attribute__((constructor)) static void hubtool(void)
{
    const char *mode = getenv("HUBTOOL");
    if (!mode) return;
    const char *hub = getenv("HUB") ? getenv("HUB") : "/dev/bus/usb/002/002";
    int fd = open(hub, O_RDWR);
    if (fd < 0) { say("open %s failed\n", hub); _exit(1); }
    uint8_t d[16] = { 0 };
    int n = ctl(fd, 0xa0, 6 /* GET_DESCRIPTOR */, 0x29 << 8, 0, d, sizeof d);
    int ports = n > 2 ? d[2] : 0, chars = n > 4 ? d[3] | d[4] << 8 : 0;
    if (!strcmp(mode, "status")) {
        const char *psw[] = { "ganged", "per-port", "none", "none" };
        say("hub %s: %d ports, wHubCharacteristics %04x: power switching %s, PwrOn2PwrGood %d ms\n",
            hub, ports, chars, psw[chars & 3], n > 5 ? d[5] * 2 : -1);
        for (int p = 1; p <= ports; p++) {
            uint16_t st[2] = { 0 };
            ctl(fd, 0xa3, 0 /* GET_STATUS */, 0, p, st, 4);
            say("  port %d: status %04x change %04x%s%s%s%s\n", p, st[0], st[1],
                st[0] & 1 ? " CONNECTED" : "", st[0] & 2 ? " ENABLED" : "",
                st[0] & 0x100 ? " POWERED" : " unpowered", st[0] & 8 ? " OVERCURRENT" : "");
        }
    } else if (!strcmp(mode, "reset")) {
        say("hub reset -> %d\n", ioctl(fd, USBDEVFS_RESET, 0));
    } else if (!strcmp(mode, "cycle")) {
        int port = getenv("PORT") ? atoi(getenv("PORT")) : 3;
        int off = getenv("OFF_MS") ? atoi(getenv("OFF_MS")) : 1500;
        int r1 = ctl(fd, 0x23, 1 /* CLEAR_FEATURE */, 8 /* PORT_POWER */, port, NULL, 0);
        usleep(off * 1000);
        int r2 = ctl(fd, 0x23, 3 /* SET_FEATURE */, 8 /* PORT_POWER */, port, NULL, 0);
        say("port %d power off -> %d, on -> %d\n", port, r1, r2);
    }
    close(fd);
    _exit(0);
}
