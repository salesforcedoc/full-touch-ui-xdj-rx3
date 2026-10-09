/*
 * touchshim-rx3.c — LD_PRELOAD shim for the RX3-native Wandboard image: the "RX3 hardware" layer, so Pioneer's own
 * rbp v1.20 runs unmodified on disk (md5 4f2efcfc0c9e3f539289f863acfddcc6; the PrimeBox-patched build works too).
 *
 * The real XDJ-RX3 reads its touch panel through Pioneer's TSC2007 driver
 * (/dev/tsc2007_2-0048). The Wandboard's ELECROW/QDTECH MPI7002 panel reports
 * touch over USB HID instead (/dev/input/eventN). This shim emulates rbp's
 * tsc2007 device from that evdev touchscreen, and stands in for the rest of the RX3 board:
 *   - front-panel SPI links (socketpair stand-ins; panel ready flag) and the "1st key" mask (no panel CPUs)
 *   - /proc/udev_* stick notices -> /tmp FIFOs (debounced bridge)
 *   - /dev/gpiodrv: GpioManager's constructor read of an input pin gets a synthetic level 1 = "no fault" (init()
 *     waits on it), writes are swallowed and the USB over-current lines 126/204 always read 1 -- one shared FIFO
 *     must not let an output pin's write be taken as another pin's level; RX3 board rev 0x700 in cpuinfo
 *   - browse list drag scrolling (the only in-memory code change; checked against the expected words)
 *   - inert in every process except rbp
 *
 * Protocol / calibration are the ones proven on the Debian setup (fbshim-hid.c):
 *   frame = 6 bytes {down, 0, x lo, x hi, y lo, y hi} in rbp's raw ADC space;
 *   rbp applies its RX3 factory calibration (commRxDataProc @0x2d74b0):
 *     calX = 1280 - (rawX - 37) * 1280 / 3976      calY = (rawY - 72) * 800 / 3856
 *   so for a screen point (sx, sy): rawX = 37 + (1280 - sx) * 3976 / 1280,
 *                                   rawY = 72 + sy * 3856 / 800.
 *
 * Env: TSC_EVDEV=/dev/input/eventN (force device), TSC_LOG=1,
 *      TSC_RAW_XMIN/XMAX/YMIN/YMAX (panel raw range, default 0/1024/0/600).
 *
 * Build: see build-shims.sh (links against the RX3 rootfs glibc 2.13).
 */
#define _GNU_SOURCE
#include <stdint.h>          /* must precede the evdev structs below: glibc's other headers no longer imply it */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/types.h>

#define TSC_DEVICE "/dev/tsc2007_2-0048"
#define TSC_MAX_X 3
#define TSC_MAX_Y 3900
#define RBP_W 1280
#define RBP_H 800
#define MIN_DOWN_FRAMES 6        /* rbp's TouchAdValueHysteresis needs several frames */
#define DOWN_FRAME_US   10000    /* ~ real tsc2007 poll period */

/* evdev ABI with 32-bit timeval (glibc 2.13 armel), independent of libc headers */
struct ev32 { int32_t sec, usec; uint16_t type, code; int32_t value; };
struct absinfo32 { int32_t value, minimum, maximum, fuzz, flat, resolution; };
#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3
#define SYN_REPORT 0
#define BTN_TOUCH 0x14a
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39
#define EVIOCGABS(abs) (0x80184540u + (abs))   /* _IOR('E', 0x40+abs, 24 bytes) */

static int active;               /* 1 only inside rbp */
static int logon;
static int evfd = -1;
static int out_pipe[2] = { -1, -1 };
static int fake_fd[64];
static int use_mt;
static int raw_xmin, raw_xmax, raw_ymin, raw_ymax;   /* the chosen panel's own digitizer grid */
static pthread_t reader_tid;
static int reader_started;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

static int sys_open(const char *p, int fl) { return syscall(SYS_openat, AT_FDCWD, p, fl, 0); }
static long sys_read(int fd, void *b, size_t n) { return syscall(SYS_read, fd, b, n); }
static int sys_close(int fd) { return syscall(SYS_close, fd); }
static int sys_ioctl(int fd, unsigned long r, void *a) { return syscall(SYS_ioctl, fd, r, a); }

#include "rbp_process.h"

static int env_int(const char *n, int d) { const char *s = getenv(n); return (s && *s) ? atoi(s) : d; }

/* Panel link ready flag (needed by Pioneer's original rbp; harmless with the PrimeBox build).
 * ui::PanelComPeerLinux::postMessage() queues the message and then waits (1 ms sleeps) until the panel thread
 * has set its ready byte (+136, set in run() once its epoll set is built). At startup a thread posts before
 * that, and the wait holds up the startup that would let run() get going: the panel thread never starts, so
 * no UI timer fires and USB mount notices are never handled (det_usb1 stays 0). On a real RX3 the panel CPU
 * answers before anyone posts. The panel-link object is the singleton at PANEL_PEER (vtable checked); set
 * its ready byte as soon as it exists: an early message then just waits in the queue for the thread.
 * (PrimeBox solved the same thing by patching postMessage / isOnMessageThread.) TSC_NO_PANEL_READY=1 off. */
#define PANEL_PEER       0x026870b0u              /* rbp v1.20: ui::PanelComPeer* (both builds) */
#define PANEL_PEER_VT    0x004d5e00u              /* vtable for ui::PanelComPeer + 8 */
#define PANEL_READY_OFF  136
#define UI_OBJ_MGR       0x02685f2cu              /* UI object manager global (as knobshim) */
#define KEY_MANAGER_OFF  100                      /* IKeyManager* at mgr + 100 */
#define IKEYMGR_VT       0x004d7a28u              /* vtable for ui::KeyManager + 8 */
#define KEY_1ST_PENDING_OFF 164
static void *panel_ready_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3000; i++) {             /* up to 60 s */
        uint32_t o = *(volatile uint32_t *)PANEL_PEER;
        if (o && *(volatile uint32_t *)o == PANEL_PEER_VT) {
            volatile uint8_t *ready = (volatile uint8_t *)(o + PANEL_READY_OFF);
            int was = *ready;
            if (!was) *ready = 1;
            if (logon) { char b[96]; int n = snprintf(b, sizeof b, "touchshim: panel link ready flag %s\n", was ? "already set" : "set"); write(2, b, n); }
            break;
        }
        usleep(20000);
    }
    /* First key state. uif::IKeyManager drops every key (onKey returns at once) while its "1st key pending" mask
     * (+164, one bit per front-panel / jog CPU, from the constructor argument) is non-zero; each CPU's first key
     * report clears its bit via notify1stKeyHandled(). There are no panel CPUs here: clear the mask as soon as the
     * key manager exists (= constructed with no panel CPUs to wait for). Not via notify1stKeyHandled(): at 0 that
     * also runs the "all first keys handled" listeners, which then wait for real panel key data and keys stay
     * dead. (PrimeBox did the same by constructing it with 0.) */
    for (int i = 0; i < 3000; i++) {             /* up to 60 s */
        uint32_t mgr = *(volatile uint32_t *)UI_OBJ_MGR;
        uint32_t km = mgr ? *(volatile uint32_t *)(mgr + KEY_MANAGER_OFF) : 0;
        if (km && *(volatile uint32_t *)km == IKEYMGR_VT) {
            int mask = *(volatile int *)(km + KEY_1ST_PENDING_OFF);
            if (mask) *(volatile int *)(km + KEY_1ST_PENDING_OFF) = 0;
            if (logon) { char b[96]; int n = snprintf(b, sizeof b, "touchshim: 1st key pending mask %#x cleared\n", mask); write(2, b, n); }
            return NULL;
        }
        usleep(20000);
    }
    return NULL;
}

/* Browse list drag scrolling (the one in-memory code change; Pioneer's rbp v1.20 only).
 * ui::touch_panel::TouchAreaProc_ListScroll::holdTouch() sends the list-scroll key only after 6 hold samples and
 * then clears its touch-on mark (+48) until the next touch-on, so a drag moves the list once and stops. Make every
 * hold sample of a touch send the scroll key and keep the mark: 0x363774 "ldr r3,[r4,#48]" -> b 0x36381c (send),
 * 0x363794 "str r5,[r4,#48]" -> nop. Both words are checked first, so another build is left alone. (PrimeBox
 * found and patched the same spot.) TSC_NO_LIST_DRAG=1 off. */
static void list_drag_fix(void)
{
    volatile uint32_t *a = (volatile uint32_t *)0x00363774, *b = (volatile uint32_t *)0x00363794;
    if (getenv("TSC_NO_LIST_DRAG") || *a != 0xe5943030u || *b != 0xe5845030u) return;
    if (syscall(SYS_mprotect, (void *)0x363000, 0x1000, 7 /* RWX */) != 0) return;
    *a = 0xea000000u | (((0x0036381cu - (0x00363774u + 8)) >> 2) & 0xffffffu);   /* b 0x36381c */
    *b = 0xe320f000u;                                                            /* nop */
    syscall(SYS_mprotect, (void *)0x363000, 0x1000, 5 /* R-X */);
    syscall(0xf0002 /* __ARM_NR_cacheflush */, 0x363774, 0x363798, 0);
    if (logon) { char m[64]; int n = snprintf(m, sizeof m, "touchshim: list drag scrolling on\n"); write(2, m, n); }
}

__attribute__((constructor)) static void init(void)
{
    active = rbp_process_check();
    logon = getenv("TSC_LOG") != NULL;
    if (active && logon) fprintf(stderr, "[touchshim] active in rbp\n");
    if (active) list_drag_fix();
    if (active && !getenv("TSC_NO_PANEL") && !getenv("TSC_NO_PANEL_READY")) {
        pthread_t t;
        if (pthread_create(&t, NULL, panel_ready_thread, NULL) == 0) pthread_detach(t);
    }
}

static int probe(const char *path)
{
    struct absinfo32 a;
    int fd = sys_open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;
    int ok = 0, xmin = 0, xmax = 0, ymin = 0, ymax = 0, mt = 0;
    if (sys_ioctl(fd, EVIOCGABS(ABS_X), &a) == 0) {
        xmin = a.minimum; xmax = a.maximum; mt = 0;
        if (sys_ioctl(fd, EVIOCGABS(ABS_Y), &a) == 0) { ymin = a.minimum; ymax = a.maximum; ok = 1; }
    } else if (sys_ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &a) == 0) {
        xmin = a.minimum; xmax = a.maximum; mt = 1;
        if (sys_ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &a) == 0) { ymin = a.minimum; ymax = a.maximum; ok = 1; }
    }
    sys_close(fd);
    /* Take the raw range from the panel itself. It is not a constant: the MPI7002 grid is 1024x600, the
     * 15.6" TSTP CTouch panel that ended up on this board reports 1920x1080. Assuming the former for the
     * latter squashes every touch in the right/bottom ~47% of the glass onto the screen's far edges. */
    if (ok) { use_mt = mt; raw_xmin = xmin; raw_xmax = xmax; raw_ymin = ymin; raw_ymax = ymax; }
    return ok;
}

static int open_touch(void)
{
    static char p[32];
    const char *forced = getenv("TSC_EVDEV");
    if (forced && *forced) return probe(forced) ? sys_open(forced, O_RDONLY) : -1;
    for (int i = 0; i < 16; i++) {
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        if (probe(p)) {
            if (logon) fprintf(stderr, "[touchshim] touch device %s (%s) raw %d..%d x %d..%d\n",
                               p, use_mt ? "MT" : "ABS", raw_xmin, raw_xmax, raw_ymin, raw_ymax);
            return sys_open(p, O_RDONLY);
        }
    }
    return -1;
}

/* provided by uishim-rx3.so when loaded (weak: NULL otherwise) */
extern int uishim_touch(int x, int y, int down) __attribute__((weak));
static int scr_x, scr_y;         /* last touch in rbp screen pixels (1280x800) */
static void to_raw(int rx, int ry, int *lx, int *ly)
{
    /* The digitizer reports in the panel's own grid (read from evdev by probe()); the panel shows our whole
     * 1280x800 image, so map that grid onto the full screen. TSC_RAW_* override the detected range. */
    int xmin = env_int("TSC_RAW_XMIN", raw_xmin), xmax = env_int("TSC_RAW_XMAX", raw_xmax);
    int ymin = env_int("TSC_RAW_YMIN", raw_ymin), ymax = env_int("TSC_RAW_YMAX", raw_ymax);
    if (xmax <= xmin) { xmin = 0; xmax = 1024; }
    if (ymax <= ymin) { ymin = 0; ymax = 600; }
    long sx = (long)(rx - xmin) * (RBP_W - 1) / (xmax - xmin);
    long sy = (long)(ry - ymin) * (RBP_H - 1) / (ymax - ymin);
    /* measured (9 crosshair targets over the screen, uishim's /tmp/uishim.cal): every tap landed
     * right of and below the target by a near-constant ~17 / ~10 px (no scale error) -> shift it back */
    sx += env_int("TSC_OFF_X", -17);
    sy += env_int("TSC_OFF_Y", -10);
    if (sx < 0) sx = 0;
    if (sx >= RBP_W) sx = RBP_W - 1;
    if (sy < 0) sy = 0;
    if (sy >= RBP_H) sy = RBP_H - 1;
    scr_x = (int)sx; scr_y = (int)sy;
    *lx = (int)(37 + (RBP_W - sx) * 3976 / RBP_W);
    *ly = (int)(72 + sy * 3856 / RBP_H);
}

static void push(int down, int x, int y)
{
    unsigned char b[6] = { (unsigned char)(down ? 1 : 0), 0,
        (unsigned char)(x & 0xff), (unsigned char)(x >> 8),
        (unsigned char)(y & 0xff), (unsigned char)(y >> 8) };
    if (out_pipe[1] >= 0) syscall(SYS_write, out_pipe[1], b, 6);
}

static void *reader(void *arg)
{
    struct ev32 ev;
    int rx = 0, ry = 0, flag = 0, down = 0, lx = 0, ly = 0, sent = 0;
    (void)arg;
    for (;;) {
        if (evfd < 0) { evfd = open_touch(); if (evfd < 0) { usleep(500000); continue; } }
        /* Like the real TSC2007 (polled ~10 ms): while the finger is down, keep
         * streaming the current position; rbp's TouchAdValueHysteresis expects it. */
        if (down) {
            struct pollfd pfd = { evfd, POLLIN, 0 };
            if (poll(&pfd, 1, DOWN_FRAME_US / 1000) == 0) { push(1, lx, ly); sent++; continue; }
        }
        long n = sys_read(evfd, &ev, sizeof ev);
        if (n != (long)sizeof ev) {
            if (n < 0 && errno == EINTR) continue;
            sys_close(evfd); evfd = -1; usleep(200000);   /* unplugged: re-detect */
            continue;
        }
        if (ev.type == EV_ABS) {
            /* 3.0 hid-multitouch sends only MT_TRACKING_ID/MT_POSITION_X/Y (no
             * BTN_TOUCH, no single-touch ABS_X/Y emulation); other drivers send
             * ABS_X/Y + BTN_TOUCH. Accept either. */
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) rx = ev.value;
            else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) ry = ev.value;
            else if (ev.code == ABS_MT_TRACKING_ID) flag = ev.value >= 0;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            flag = ev.value != 0;
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            /* on-screen top-row buttons (uishim-rx3): touches that start on a button are
             * consumed there and never reach rbp */
            static int ui_owned;
            int (*ui_touch)(int, int, int) = uishim_touch;
            if (flag && !down && !ui_owned && ui_touch) {
                to_raw(rx, ry, &lx, &ly);
                if (ui_touch(scr_x, scr_y, 1)) { ui_owned = 1; continue; }
            }
            if (ui_owned) {
                to_raw(rx, ry, &lx, &ly);
                if (!flag) { ui_touch(scr_x, scr_y, 0); ui_owned = 0; }
                else ui_touch(scr_x, scr_y, 2);   /* finger moved (uishim sliders, e.g. CFX PARAMETER) */
                continue;
            }
            if (flag) {
                to_raw(rx, ry, &lx, &ly);
                if (!down && logon) fprintf(stderr, "[touchshim] down raw %d,%d -> %d,%d\n", rx, ry, lx, ly);
                if (!down) sent = 0;
                down = 1;
                push(1, lx, ly); sent++;
            } else if (down) {
                /* quick tap: make sure rbp saw enough down frames to commit it */
                while (sent < MIN_DOWN_FRAMES) { usleep(DOWN_FRAME_US); push(1, lx, ly); sent++; }
                if (logon) fprintf(stderr, "[touchshim] up after %d frames\n", sent);
                push(0, lx, ly); usleep(DOWN_FRAME_US); push(0, lx, ly);
                down = 0;
            }
        }
    }
    return NULL;
}

static int tsc_open(void)
{
    pthread_mutex_lock(&lk);
    if (!reader_started) {
        if (syscall(SYS_pipe2, out_pipe, 0) == 0 && pthread_create(&reader_tid, NULL, reader, NULL) == 0)
            reader_started = 1;
    }
    pthread_mutex_unlock(&lk);
    if (!reader_started) { errno = ENODEV; return -1; }
    int fd = syscall(SYS_dup, out_pipe[0]);
    while (fd >= 0 && fd <= 2) { int n = syscall(SYS_dup, fd); sys_close(fd); fd = n; }  /* rbp treats fd 0 as failure */
    if (fd < 0 || fd >= 64) { if (fd >= 0) sys_close(fd); errno = EMFILE; return -1; }
    fake_fd[fd] = 1;
    return fd;
}

static int is_fake(int fd) { return fd >= 0 && fd < 64 && fake_fd[fd]; }

/* Front-panel SPI links (/dev/subucom_spi1.0 = panel CPU, 2.0 = second CPU). The Wandboard has no panel CPU and
 * no such nodes, so rbp's PanelComPeerLinux::run() failed to epoll them and its thread exited at once -- and that
 * thread is the only caller of uif::MsgManager::checkTimer(), so NO rbp UI timer ever fired (e.g. the Master Rec
 * 2 s guard timer stayed armed, so recording could never be stopped). Stand-in: one end of a socketpair that never
 * becomes readable (epoll works, the thread just waits and services timers); every ioctl on it (SPI setup,
 * tx_transfer) reports success. TSC_NO_PANEL=1 turns this off. */
#define PANEL_FAKE 2
static int panel_open(const char *path)
{
    int sv[2];
    if (syscall(288 /* socketpair on ARM EABI (281 is socket) */, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, sv) != 0) { errno = ENODEV; return -1; }
    int fd = sv[0];
    while (fd >= 0 && fd <= 2) { int n = syscall(SYS_dup, fd); sys_close(fd); fd = n; }
    if (fd < 0 || fd >= 64) { if (fd >= 0) sys_close(fd); sys_close(sv[1]); errno = EMFILE; return -1; }
    fake_fd[fd] = PANEL_FAKE;                      /* sv[1] stays open (no EOF / HUP), nobody writes to it */
    if (logon) { char b[96]; int n = snprintf(b, sizeof b, "touchshim: panel link %s -> fd %d\n", path, fd); write(2, b, n); }
    return fd;
}
static int is_panel_dev(const char *path)
{
    if (logon && path && strncmp(path, "/dev/subucom", 12) == 0) {
        char b[128]; int n = snprintf(b, sizeof b, "touchshim: open %s (active %d)\n", path, active); write(2, b, n);
    }
    return active && path && !getenv("TSC_NO_PANEL") &&
           (strcmp(path, "/dev/subucom_spi1.0") == 0 || strcmp(path, "/dev/subucom_spi2.0") == 0);
}

/* USB notices: Pioneer's original rbp reads the kernel queues /proc/udev_usb1/2, udev_usbctn1/2 itself (the
 * PrimeBox-patched one reads /tmp/udev_* instead). Point it at the /tmp FIFOs, which init.d's bridges
 * (usb-bridge.sh: debounced) feed from /proc -- same path for both builds. */
static const char *udev_redirect(const char *path, char *buf, size_t n)
{
    if (!active || !path || strncmp(path, "/proc/udev_usb", 14) != 0 || getenv("TSC_NO_UDEV_REDIRECT")) return path;
    snprintf(buf, n, "/tmp/%s", path + 6);
    if (logon) { char b[128]; int k = snprintf(b, sizeof b, "touchshim: %s -> %s\n", path, buf); write(2, b, k); }
    return buf;
}

/* GPIO. rbp works RX3 GPIO pins through Pioneer's /dev/gpiodrv: open, lseek to the pin number once, then 1-byte
 * reads of an *input* pin's current level (common::GpioManager::read @0x28f8c) or 1-byte writes of an *output*
 * pin's (GpioManager::write @0x28efc -> write(fd,&byte,1)). On the RX3 that node is a pin-addressed char driver,
 * so a read returns at once and a write to an output pin can never be seen by a reader of some other pin. Here
 * the node is one FIFO shared by every fd rbp opens on it, which breaks both halves of that:
 *
 * - The constructor's read of an input pin has no writer to answer it, so it blocks. ui::UsbStorageManager does
 *   that twice while ui::UiObjectManager::init() is building the UI, which hangs the main thread inside init():
 *   startUp() returns 0 before TouchPanel::openDevice(), and no touch works at all (dead screen). The ctor only
 *   reads when it was given a callback *and* the pin is an input, i.e. exactly the two over-current lines below;
 *   output-pin managers (mixerengine::MasterOut's pins 89/90, ui::PortTest's) never read. So answer the *first*
 *   read on each gpiodrv fd -- the constructor's, and with init() waiting on it -- with level 1 ("no fault") and
 *   let later reads fall through to the FIFO and block, which parks the monitor thread in read() rather than
 *   spinning it (answering every read instead lets GpioManager::run() @0x28ac8 loop at ~250k reads/s).
 *
 * - A *write* is a byte in that shared FIFO, and whatever GpioManager is parked in read() takes it as its own
 *   pin's level. The mixer's attenuator relays (mixerengine::MasterOut::setATTEN) and the panel LEDs
 *   (ui::panel_protocol::MainGpioLed::controGpio) write through 48 call sites, so switching an output off makes a
 *   reader see level 0 -- and on pins 126/204 that is the RX3's active-low USB power-switch fault:
 *   UsbStorageManager::handleGpioMessage -> notify_over_current(0) puts up "USB Error. Remove the device." and
 *   latches it. Swallow writes to a tracked gpiodrv fd, as the char driver's pins are isolated.
 *
 * Pins 126 (USB1) and 204 (USB2) are then always read as level 1 whatever the FIFO holds, so nothing can raise
 * that caution (the MEGA4 hub does its own over-current protection; pins 17/94/144 belong to PortTest, service
 * mode). The pin an fd addresses is learned from the constructor's lseek(fd, pin, SEEK_SET) -- on a FIFO lseek
 * itself fails, so it cannot be read back from the fd afterwards. TSC_NO_GPIO_FAKE=1 keeps the constructor read
 * and the over-current lines out of it, TSC_NO_GPIO_WRITE_SWALLOW=1 the writes, TSC_NO_OC_FIX=1 the tracking. */
#define GPIO_OC_USB1 126
#define GPIO_OC_USB2 204
static unsigned char gpio_fd[256];    /* fd belongs to /dev/gpiodrv */
static unsigned char gpio_first[256]; /* its constructor read has already been answered */
static unsigned char gpio_pin[256];   /* the pin it was lseeked to (255 = still unknown) */
static unsigned char gpio_oc[256];    /* over-current line: already logged */
static unsigned char gpio_wrote;      /* already logged a swallowed write */
static int track_gpio(const char *path, int fd)
{
    if (active && fd >= 0 && fd < 256 && path && strcmp(path, "/dev/gpiodrv") == 0 && !getenv("TSC_NO_OC_FIX")) {
        gpio_fd[fd] = 1;
        gpio_first[fd] = 0;   /* a fresh open is a fresh constructor */
        gpio_pin[fd] = 255;
        gpio_oc[fd] = 0;
    }
    return fd;
}
static int gpio_oc_pin(int fd) { return gpio_pin[fd] == GPIO_OC_USB1 || gpio_pin[fd] == GPIO_OC_USB2; }

ssize_t read(int fd, void *buf, size_t n)
{
    if (active && fd >= 0 && fd < 256 && gpio_fd[fd] && n >= 1 && !getenv("TSC_NO_GPIO_FAKE")) {
        if (gpio_oc_pin(fd)) {                       /* the USB power-switch fault lines never report a fault */
            ((unsigned char *)buf)[0] = 1;
            if (logon && !gpio_oc[fd]) {
                gpio_oc[fd] = 1;
                char b[80]; int k = snprintf(b, sizeof b, "touchshim: gpio fd %d (pin %d, over-current) read -> 1\n", fd, gpio_pin[fd]);
                write(2, b, k);
            }
            return 1;
        }
        if (!gpio_first[fd]) {                       /* the constructor's read: init() is waiting on it */
            gpio_first[fd] = 1;
            ((unsigned char *)buf)[0] = 1;
            if (logon) { char b[80]; int k = snprintf(b, sizeof b, "touchshim: gpio fd %d first read -> 1 (pin %d)\n", fd, gpio_pin[fd]); write(2, b, k); }
            return 1;
        }
    }
    return sys_read(fd, buf, n);                     /* anything else: block on the FIFO as before */
}

ssize_t write(int fd, const void *buf, size_t n)
{
    if (active && fd >= 0 && fd < 256 && gpio_fd[fd] && !is_fake(fd) && !getenv("TSC_NO_GPIO_WRITE_SWALLOW")) {
        if (logon && !gpio_wrote) {
            gpio_wrote = 1;
            char b[112]; int k = snprintf(b, sizeof b, "touchshim: gpio fd %d (pin %d) write of %d swallowed: a shared FIFO would hand it to the readers\n",
                                          fd, gpio_pin[fd], n ? ((const unsigned char *)buf)[0] : 0);
            write(2, b, k);
        }
        return (ssize_t)n;                           /* an output pin: no reader of another pin may see it */
    }
    return syscall(SYS_write, fd, buf, n);
}

off_t lseek(int fd, off_t off, int whence)
{
    if (active && fd >= 0 && fd < 256 && gpio_fd[fd] && whence == SEEK_SET && off >= 0 && off < 255) {
        gpio_pin[fd] = (unsigned char)off;            /* the constructor selects the pin here */
        if (logon) { char b[64]; int k = snprintf(b, sizeof b, "touchshim: gpio fd %d -> pin %d\n", fd, (int)off); write(2, b, k); }
    }
    return syscall(SYS_lseek, fd, off, whence);
}

/* RX3 board revision. rbp (juce::SystemStats::getCpuRevision) reads "Revision" from /proc/cpuinfo and
 * board_is_rev() compares bits 0xf00 with the RX3 boards it knows (0x700, 0x600); the ALSA scan picks its audio
 * devices by it, and with neither match there is no output device: decks load but never play. Pioneer's kernel
 * reports the RX3 board revision there; ours reports the i.MX6 one (69010). Give rbp a copy of cpuinfo with
 * bits 0xf00 = TSC_BOARD_REV (default 0x700, the board PrimeBox's edit selected). TSC_BOARD_REV=0 off. */
static const char *cpuinfo_copy(void)
{
    static int made;
    const char *out = "/tmp/rx3-cpuinfo";
    if (made) return out;
    int rev = getenv("TSC_BOARD_REV") ? (int)strtol(getenv("TSC_BOARD_REV"), NULL, 0) : 0x700;
    if (!rev) return NULL;
    char in[4096]; int fd = sys_open("/proc/cpuinfo", O_RDONLY), n = 0, k;
    if (fd < 0) return NULL;
    while (n < (int)sizeof in - 1 && (k = sys_read(fd, in + n, sizeof in - 1 - n)) > 0) n += k;
    sys_close(fd);
    in[n] = 0;
    char *r = strstr(in, "\nRevision");
    if (!r) return NULL;
    char *colon = strchr(r, ':'), *eol = colon ? strchr(colon, '\n') : NULL;
    if (!colon || !eol) return NULL;
    long v = strtol(colon + 1, NULL, 16);
    v = (v & ~0xf00L) | (rev & 0xf00);
    char buf[4200];
    int m = snprintf(buf, sizeof buf, "%.*s: %lx%s", (int)(colon - in), in, v, eol);
    int o = syscall(SYS_openat, AT_FDCWD, out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (o < 0) return NULL;
    syscall(SYS_write, o, buf, m); sys_close(o);
    if (logon) { char b[80]; int t = snprintf(b, sizeof b, "touchshim: cpuinfo Revision -> %lx\n", v); write(2, b, t); }
    made = 1;
    return out;
}
extern FILE *fopen64(const char *, const char *);
FILE *fopen(const char *path, const char *mode)
{
    if (active && path && strcmp(path, "/proc/cpuinfo") == 0) { const char *c = cpuinfo_copy(); if (c) path = c; }
    return fopen64(path, mode);                  /* glibc's (same as fopen with O_LARGEFILE); avoids dlsym */
}

int open(const char *path, int flags, ...)
{
    char ub[48]; path = udev_redirect(path, ub, sizeof ub);
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (active && path && strcmp(path, TSC_DEVICE) == 0) return tsc_open();
    if (is_panel_dev(path)) return panel_open(path);
    return track_gpio(path, syscall(SYS_openat, AT_FDCWD, path, flags, mode));
}
int open64(const char *path, int flags, ...)
{
    char ub[48]; path = udev_redirect(path, ub, sizeof ub);
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (active && path && strcmp(path, TSC_DEVICE) == 0) return tsc_open();
    if (is_panel_dev(path)) return panel_open(path);
    return track_gpio(path, syscall(SYS_openat, AT_FDCWD, path, flags | 0400000 /* O_LARGEFILE on ARM */, mode));
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (is_fake(fd) && fake_fd[fd] == PANEL_FAKE) return 0;   /* panel link: setup / transfers succeed */
    if (is_fake(fd)) {
        switch (req) {
        case 0x80046b00: if (arg) *(unsigned int *)arg = TSC_MAX_X; return 0;
        case 0x80026b01: if (arg) *(unsigned short *)arg = TSC_MAX_Y; return 0;
        default: return 0;   /* 0x40046b00 / 0x40026b01 setters: accept */
        }
    }
    return sys_ioctl(fd, req, arg);
}

int close(int fd)
{
    if (is_fake(fd)) fake_fd[fd] = 0;
    if (fd >= 0 && fd < 256) { gpio_fd[fd] = 0; gpio_first[fd] = 0; gpio_pin[fd] = 255; gpio_oc[fd] = 0; }
    return sys_close(fd);
}
