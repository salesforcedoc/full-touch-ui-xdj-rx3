/*
 * audioshim-rx3.c — LD_PRELOAD ALSA shim for the RX3-native Wandboard image.
 *
 * rbp (XDJ-RX3) opens its on-board DACs by name — "hw:cs4344audiorev8,0/1/2"
 * (playback+capture, 2ch S24_LE 44.1 kHz, 64-frame periods, duplex threads) plus
 * "hw:esaics4344audio,0" (capture) — none of which exist on the Wandboard. This shim
 * presents each named device as a virtual PCM that behaves like hardware:
 *   - writei() blocks until the mixer has drained room (paces rbp's output threads)
 *   - readi() returns silence paced by the same clock (rbp reads+writes per thread)
 * One mixer thread pulls a period from every playback stream, routes it by role onto
 * the DDJ-FLX4's 4 output channels (1/2 = master, 3/4 = headphones) and writes that to
 * the FLX4, whose clock paces everything. Without an FLX4 the mixer keeps time as a
 * null sink and reopens the FLX4 when it appears.
 *
 * Roles are by DEVICE NAME, never by open order (the Debian audioshim guessed by
 * order and misrouted). Map: env AUDIO_MAP="0:X,1:Y,2:Z", X/Y/Z in {master,hp,none}.
 * Log: /tmp/audioshim.log (opens, formats, per-device peak levels every ~2 s).
 *
 * Build: build-shims.sh (links the RX3 rootfs glibc 2.13). 32-bit time_t structs and
 * syscalls are used explicitly because the musl headers default to 64-bit time_t.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/syscall.h>
#include "rbp_process.h"

extern void *dlvsym(void *, const char *, const char *);   /* glibc libdl; not in musl headers */

typedef void snd_pcm_t;
typedef void snd_pcm_hw_params_t;
typedef void snd_pcm_sw_params_t;
typedef void snd_ctl_t;
typedef void snd_pcm_info_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

#define FMT_S16_LE   2
#define FMT_S24_LE   6
#define FMT_S32_LE   10
#define FMT_S24_3LE  32
#define STREAM_PLAYBACK 0

struct ts32 { int32_t sec, nsec; };          /* glibc 2.13 armel timespec */
#ifndef SYS_clock_gettime
#define SYS_clock_gettime 263                  /* ARM EABI, 32-bit time (kernel 3.0) */
#endif
/* glibc 2.13 symbol with a 32-bit timespec (musl would redirect to a *_time64 symbol) */
extern int cond_timedwait32(pthread_cond_t *, pthread_mutex_t *, const struct ts32 *) __asm__("pthread_cond_timedwait");

/* ---------------- logging / time ---------------- */
static void alog(const char *fmt, ...)
{
    char b[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    int fd = syscall(SYS_openat, AT_FDCWD, "/tmp/audioshim.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    if (syscall(SYS_lseek, fd, 0, SEEK_END) > 64 * 1024) syscall(SYS_ftruncate, fd, 0);   /* /tmp is 512 KB */
    syscall(SYS_write, fd, b, n > 0 && n < (int)sizeof b ? n : (int)strlen(b));
    syscall(SYS_close, fd);
}
static int64_t now_us(void)
{
    struct ts32 t; syscall(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, &t);
    return (int64_t)t.sec * 1000000 + t.nsec / 1000;
}
static void deadline_50ms(struct ts32 *t)
{
    syscall(SYS_clock_gettime, 0 /* CLOCK_REALTIME */, t);
    t->nsec += 50000000;
    if (t->nsec >= 1000000000) { t->sec++; t->nsec -= 1000000000; }
}

/* ---------------- real ALSA ---------------- */
int snd_pcm_open(snd_pcm_t **, const char *, int, int);
int snd_pcm_close(snd_pcm_t *);
int snd_pcm_hw_params_any(snd_pcm_t *, snd_pcm_hw_params_t *);
int snd_pcm_hw_params_set_access(snd_pcm_t *, snd_pcm_hw_params_t *, int);
int snd_pcm_hw_params_set_format(snd_pcm_t *, snd_pcm_hw_params_t *, int);
int snd_pcm_hw_params_set_channels(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned *, int *);
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t *, int *);
int snd_pcm_hw_params_set_periods_near(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned *, int *);
int snd_pcm_hw_params_test_rate(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned, int);
int snd_pcm_hw_params_get_channels_min(const snd_pcm_hw_params_t *, unsigned *);
int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *, unsigned *);
int snd_pcm_hw_params(snd_pcm_t *, snd_pcm_hw_params_t *);
int snd_pcm_sw_params_current(snd_pcm_t *, snd_pcm_sw_params_t *);
int snd_pcm_sw_params_get_boundary(const snd_pcm_sw_params_t *, snd_pcm_uframes_t *);
int snd_pcm_sw_params_set_silence_threshold(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t);
int snd_pcm_sw_params_set_silence_size(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t);
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t);
int snd_pcm_sw_params_set_stop_threshold(snd_pcm_t *, snd_pcm_sw_params_t *, snd_pcm_uframes_t);
int snd_pcm_sw_params(snd_pcm_t *, snd_pcm_sw_params_t *);
int snd_pcm_prepare(snd_pcm_t *);
int snd_pcm_link(snd_pcm_t *, snd_pcm_t *);
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *, const void *, snd_pcm_uframes_t);
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *, void *, snd_pcm_uframes_t);
int snd_ctl_open(snd_ctl_t **, const char *, int);
int snd_ctl_close(snd_ctl_t *);
int snd_ctl_pcm_info(snd_ctl_t *, snd_pcm_info_t *);
const char *snd_strerror(int);

#define R(name) static __typeof__(&name) real_##name
R(snd_pcm_open); R(snd_pcm_close); R(snd_pcm_hw_params_any); R(snd_pcm_hw_params_set_access);
R(snd_pcm_hw_params_set_format); R(snd_pcm_hw_params_set_channels); R(snd_pcm_hw_params_set_rate_near);
R(snd_pcm_hw_params_set_period_size_near); R(snd_pcm_hw_params_set_periods_near);
R(snd_pcm_hw_params_test_rate); R(snd_pcm_hw_params_get_channels_min); R(snd_pcm_hw_params_get_channels_max);
R(snd_pcm_hw_params); R(snd_pcm_sw_params_current); R(snd_pcm_sw_params_get_boundary);
R(snd_pcm_sw_params_set_silence_threshold); R(snd_pcm_sw_params_set_silence_size);
R(snd_pcm_sw_params_set_start_threshold); R(snd_pcm_sw_params_set_stop_threshold);
R(snd_pcm_sw_params); R(snd_pcm_prepare); R(snd_pcm_link); R(snd_pcm_writei); R(snd_pcm_readi);
R(snd_ctl_open); R(snd_ctl_close); R(snd_ctl_pcm_info); R(snd_strerror);

static int active = -1;
static void init_real(void)
{
    static int done;
    if (done) return;
    done = 1;
#define G(n) real_##n = dlsym(RTLD_NEXT, #n)
    /* These exist in two ABIs; rbp uses the ALSA_0.9.0rc4 (pointer) versions and a bare
     * dlsym can return the old ALSA_0.9 by-value ones (pointer read as a huge value ->
     * the FLX4 came up at 48 kHz with a 43690-frame period). Bind the versions explicitly. */
#define GV(n) do { real_##n = dlvsym(RTLD_NEXT, #n, "ALSA_0.9.0rc4"); if (!real_##n) real_##n = dlsym(RTLD_NEXT, #n); } while (0)
    G(snd_pcm_open); G(snd_pcm_close); G(snd_pcm_hw_params_any); G(snd_pcm_hw_params_set_access);
    G(snd_pcm_hw_params_set_format); G(snd_pcm_hw_params_set_channels); GV(snd_pcm_hw_params_set_rate_near);
    GV(snd_pcm_hw_params_set_period_size_near); GV(snd_pcm_hw_params_set_periods_near);
    G(snd_pcm_hw_params_test_rate); GV(snd_pcm_hw_params_get_channels_min); GV(snd_pcm_hw_params_get_channels_max);
    G(snd_pcm_hw_params); G(snd_pcm_sw_params_current); G(snd_pcm_sw_params_get_boundary);
    G(snd_pcm_sw_params_set_silence_threshold); G(snd_pcm_sw_params_set_silence_size);
    G(snd_pcm_sw_params_set_start_threshold); G(snd_pcm_sw_params_set_stop_threshold);
    G(snd_pcm_sw_params); G(snd_pcm_prepare); G(snd_pcm_link); G(snd_pcm_writei); G(snd_pcm_readi);
    G(snd_ctl_open); G(snd_ctl_close); G(snd_ctl_pcm_info); G(snd_strerror);
#undef G
#undef GV
}
static int check_active(void)
{
    if (active < 0) { active = rbp_process_check(); init_real(); }
    return active;
}

/* ---------------- virtual RX3 devices ---------------- */
enum { ROLE_NONE = 0, ROLE_MASTER = 1, ROLE_HP = 2 };
static volatile int hp_invert;
static const char *role_name[] = { "none", "master", "hp" };
#define VMAGIC 0x52583356u   /* "RX3V" */
#define RING   8192          /* frames per stream (stereo int32), power of two */
#define MAXV   8

struct vpcm {
    uint32_t magic;
    char name[48];
    int dev, stream, role;
    unsigned ch, rate, fmt;
    snd_pcm_uframes_t period;
    int32_t ring[RING * 2];
    unsigned rd, wr;                 /* frame counters (index = counter & (RING-1)) */
    int32_t peak, peak_l, peak_r;     /* peak_l / peak_r: per channel, logged for the headphone feed */
    unsigned long frames;
    int open;
};
static struct vpcm vdev[MAXV];
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int mixer_started;
static unsigned g_rate = 0;          /* stream rate (first hw_params wins) */
static unsigned g_clock = 0;         /* frames output by the mixer (the FLX4 clock) */

/* params objects -> vpcm (get_channels_min/max, get_boundary get no pcm argument) */
static struct { const void *params; struct vpcm *v; } ptab[32];
static void ptab_set(const void *p, struct vpcm *v)
{
    for (int i = 0; i < 32; i++) if (ptab[i].params == p || !ptab[i].params) { ptab[i].params = p; ptab[i].v = v; return; }
}
static struct vpcm *ptab_get(const void *p)
{
    for (int i = 0; i < 32; i++) if (ptab[i].params == p) return ptab[i].v;
    return NULL;
}

static struct vpcm *V(const void *pcm)
{
    struct vpcm *v = (struct vpcm *)pcm;
    return (v >= vdev && v < vdev + MAXV && v->magic == VMAGIC) ? v : NULL;
}

static int is_rx3_name(const char *n)
{
    return n && (strstr(n, "cs4344audio") || strstr(n, "ak4384audio") || strstr(n, "esaics4344") || strstr(n, "esaiak4384"));
}

static int role_for(int dev)
{
    int role = dev == 0 ? ROLE_MASTER : dev == 1 ? ROLE_HP : ROLE_NONE;   /* default until traced */
    const char *m = getenv("AUDIO_MAP");
    if (m) {
        char key[4]; snprintf(key, sizeof key, "%d:", dev);
        const char *p = strstr(m, key);
        if (p) {
            p += strlen(key);
            role = !strncmp(p, "master", 6) ? ROLE_MASTER : !strncmp(p, "hp", 2) ? ROLE_HP : ROLE_NONE;
        }
    }
    return role;
}

static int32_t to_s24(const void *buf, unsigned fmt, unsigned idx)
{
    switch (fmt) {
    case FMT_S16_LE: return (int32_t)((const int16_t *)buf)[idx] << 8;
    case FMT_S32_LE: return ((const int32_t *)buf)[idx] >> 8;
    case FMT_S24_3LE: { const uint8_t *b = (const uint8_t *)buf + idx * 3;
        return (int32_t)((uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24) >> 8; }
    default: /* S24_LE: 24 bits in the low bits of 32 */
        return (int32_t)(((const uint32_t *)buf)[idx] << 8) >> 8;
    }
}

/* ---------------- mixer: virtual streams -> FLX4 ---------------- */
static snd_pcm_t *flx;
static int flx_fmt;
static snd_pcm_uframes_t PER = 256;

static int flx_open(void)
{
    const char *dev = getenv("AUDIO_DEV") ? getenv("AUDIO_DEV") : "hw:DDJFLX4,0";
    snd_pcm_t *p;
    int e = real_snd_pcm_open(&p, dev, STREAM_PLAYBACK, 0);
    if (e < 0) return e;
    char hwp[1024] __attribute__((aligned(8)));   /* > snd_pcm_hw_params_sizeof() */
    unsigned rate = g_rate ? g_rate : 44100, periods = 4; int dir = 0;
    snd_pcm_uframes_t per = PER;
    real_snd_pcm_hw_params_any(p, hwp);
    real_snd_pcm_hw_params_set_access(p, hwp, 3 /* RW_INTERLEAVED */);
    flx_fmt = FMT_S24_3LE;
    if (real_snd_pcm_hw_params_set_format(p, hwp, flx_fmt) < 0) { flx_fmt = FMT_S16_LE; real_snd_pcm_hw_params_set_format(p, hwp, flx_fmt); }
    real_snd_pcm_hw_params_set_channels(p, hwp, 4);
    real_snd_pcm_hw_params_set_rate_near(p, hwp, &rate, &dir);
    real_snd_pcm_hw_params_set_period_size_near(p, hwp, &per, &dir);
    real_snd_pcm_hw_params_set_periods_near(p, hwp, &periods, &dir);
    e = real_snd_pcm_hw_params(p, hwp);
    if (e < 0) { real_snd_pcm_close(p); return e; }
    real_snd_pcm_prepare(p);
    flx = p; PER = per;
    alog("audioshim: FLX4 %s open fmt=%d rate=%u period=%lu periods=%u\n", dev, flx_fmt, rate, per, periods);
    return 0;
}

/* HEADPHONES MONO SPLIT (SHORTCUT / UTILITY): rbp then sends the cue in mono on its HP bus's left channel and
 * leaves the right one exactly silent -- on the RX3 the hardware adds the master there. Here the right channel of
 * the cue feed gets the master (mono, with the same polarity as the rest of the cue feed, see hp_invert). The mode is read from
 * rbp's HP bus itself: left has signal and right is all zero = MONO SPLIT; right has signal = STEREO; nothing cued =
 * keep the last mode. (A settings byte tried first did not follow the setting.) `touch /tmp/audioshim.nosplit` or
 * AUDIO_NO_SPLIT=1 turns the fill off. */
static volatile int hp_split;                 /* 1 = MONO SPLIT seen on rbp's HP bus */
int audioshim_hp_split(void) { return hp_split; }   /* for knobshim: FLX4 master path only in STEREO */
/* MASTER CUE (FLX4 button, tracked by knobshim): the RX3's hardware adds the master to the headphone cue; rbp's
 * cue feed never contains it, so it is added here (same polarity as the cue feed). */
extern int knobshim_master_cue(void) __attribute__((weak));

static void *mixer(void *arg)
{
    (void)arg;
    static int32_t mix[2048 * 4];
    static uint8_t out[2048 * 4 * 3];
    int64_t next = now_us(), last_log = next, last_try = 0;
    unsigned clk0 = 0, under = 0;
    /* like rbp's own JuceALSA thread: SCHED_FIFO on the isolated core (isolcpus=3) */
    {
        unsigned long mask = 1UL << (getenv("AUDIO_CPU") ? atoi(getenv("AUDIO_CPU")) : 3);
        syscall(SYS_sched_setaffinity, 0, sizeof mask, &mask);
        struct { int prio; } sp = { 90 };
        syscall(SYS_sched_setscheduler, 0, 1 /* SCHED_FIFO */, &sp);
    }
    for (;;) {
        if (!flx && now_us() - last_try > 2000000) { last_try = now_us(); if (flx_open() < 0) flx = NULL; }
        snd_pcm_uframes_t n = PER > 2048 ? 2048 : PER;
        memset(mix, 0, sizeof(int32_t) * 4 * n);
        pthread_mutex_lock(&mx);
        for (int i = 0; i < MAXV; i++) {
            struct vpcm *v = &vdev[i];
            if (!v->open || v->stream != STREAM_PLAYBACK) continue;
            unsigned avail = v->wr - v->rd, take = avail < n ? avail : n;
            if (take < n && v->frames) under++;
            int o = v->role == ROLE_MASTER ? 0 : v->role == ROLE_HP ? 2 : -1;
            int32_t hl = 0, hr = 0;                /* HP bus: any signal left / right this period */
            for (unsigned f = 0; f < take; f++) {
                unsigned k = ((v->rd + f) & (RING - 1)) * 2;
                int32_t l = v->ring[k], r = v->ring[k + 1];
                int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
                if (al > v->peak) v->peak = al;
                if (ar > v->peak) v->peak = ar;
                if (al > v->peak_l) v->peak_l = al;
                if (ar > v->peak_r) v->peak_r = ar;
                hl |= l; hr |= r;
                if (o == 2 && hp_invert) { l = -l; r = -r; }
                if (o >= 0) { mix[f * 4 + o] += l; mix[f * 4 + o + 1] += r; }
            }
            if (o == 2 && take) {
                /* MONO SPLIT / STEREO: rbp's engine state, mixerengine::HeadPhone::stereoType (MixerEngine singleton
                 * *0x011493c0 +72 -> HeadPhone +200: 0 = MONO SPLIT, 1 = STEREO; set by HeadPhone::setStereoType).
                 * Fallback without it: infer from the HP bus (cue only on the left = split). */
                uint8_t *eng = *(uint8_t **)0x011493c0, *hp = eng ? *(uint8_t **)(eng + 72) : 0;
                if (hp) hp_split = *(volatile int *)(hp + 200) == 0;
                else if (hr) hp_split = 0;
                else if (hl) hp_split = 1;
            }
            v->rd += take;
        }
        {                                          /* MONO SPLIT: master (mono) into the cue feed's right ear */
            static int was = -1, off = -1;
            static int64_t chk;
            if (off < 0) off = getenv("AUDIO_NO_SPLIT") && atoi(getenv("AUDIO_NO_SPLIT"));
            static int nofile;
            if (now_us() - chk > 1000000) { chk = now_us(); nofile = access("/tmp/audioshim.nosplit", F_OK) == 0; }
            int split = hp_split && !off && !nofile;
            if (split != was) { was = split; alog("audioshim: headphones %s\n", split ? "MONO SPLIT (R = master)" : "STEREO"); }
            if (split)                             /* same polarity as the rest of the cue feed (hp_invert) */
                for (unsigned f = 0; f < n; f++) {
                    int32_t m = (mix[f * 4] >> 1) + (mix[f * 4 + 1] >> 1);
                    mix[f * 4 + 3] = hp_invert ? -m : m;
                }
            static int mc_was = -1, mcflip = 0, mcflip_was = -1;
            static int64_t mchk;
            if (now_us() - mchk > 500000) { mchk = now_us(); mcflip = access("/tmp/audioshim.mcinv", F_OK) == 0; }
            if (mcflip != mcflip_was) { mcflip_was = mcflip; alog("audioshim: master-cue polarity %s\n", mcflip ? "flipped" : "default"); }
            int mneg = hp_invert ^ mcflip;             /* polarity of the master added for MASTER CUE */
            int mc = knobshim_master_cue ? knobshim_master_cue() : 0;
            if (mc != mc_was) { mc_was = mc; alog("audioshim: master cue %s\n", mc ? "on (master added to the cue feed)" : "off"); }
            if (mc)                                /* MASTER CUE: master into the cue (left only in MONO SPLIT) */
                for (unsigned f = 0; f < n; f++) {
                    int32_t ml = mix[f * 4], mr = mix[f * 4 + 1];
                    if (split) { int32_t m = (ml >> 1) + (mr >> 1); mix[f * 4 + 2] += mneg ? -m : m; }
                    else { mix[f * 4 + 2] += mneg ? -ml : ml; mix[f * 4 + 3] += mneg ? -mr : mr; }
                }
        }
        g_clock += n;
        pthread_cond_broadcast(&cv);
        if (now_us() - last_log > 2000000) {
            /* headphone feed polarity: the FLX4 mixes the cue feed (USB 3/4) with the master feed
             * in hardware, and with rbp's HP bus as-is the two partly cancelled (dip as the channel
             * fader rises / at the centre of HEADPHONE MIX). Inverted by default (verified by ear);
             * AUDIO_HP_INVERT=0 disables, /tmp/audioshim.hpinv flips it at runtime. */
            int inv = !(getenv("AUDIO_HP_INVERT") && !atoi(getenv("AUDIO_HP_INVERT"))) ^ (access("/tmp/audioshim.hpinv", F_OK) == 0);
            if (inv != hp_invert) { hp_invert = inv; alog("audioshim: hp feed %s\n", inv ? "inverted" : "normal"); }
            int64_t el = now_us() - last_log;
            last_log = now_us();
            char b[400]; int p = 0;
            p += snprintf(b + p, sizeof b - p, " clock=%lld/s underruns=%u", (long long)(g_clock - clk0) * 1000000 / el, under);
            clk0 = g_clock; under = 0;
            for (int i = 0; i < MAXV; i++) if (vdev[i].open && vdev[i].stream == STREAM_PLAYBACK) {
                p += snprintf(b + p, sizeof b - p, " [%s %s peak=%d", vdev[i].name, role_name[vdev[i].role], (int)vdev[i].peak);
                if (vdev[i].role == ROLE_HP)       /* L / R separately (MONO SPLIT: L = cue, R = master) */
                    p += snprintf(b + p, sizeof b - p, " L=%d R=%d", (int)vdev[i].peak_l, (int)vdev[i].peak_r);
                p += snprintf(b + p, sizeof b - p, "]");
                vdev[i].peak = vdev[i].peak_l = vdev[i].peak_r = 0;
            }
            if (p) alog("audioshim: levels%s flx=%s\n", b, flx ? "on" : "off");
        }
        pthread_mutex_unlock(&mx);

        /* debug: `touch /tmp/audioshim.tone` adds 5 s of -12 dBFS 1 kHz to ch1/2 (master) */
        {
            static long tleft; static unsigned tph;
            if (!tleft && syscall(SYS_faccessat, AT_FDCWD, "/tmp/audioshim.tone", 0, 0) == 0) {
                syscall(SYS_unlinkat, AT_FDCWD, "/tmp/audioshim.tone", 0);
                tleft = (long)(g_rate ? g_rate : 44100) * 5;
                alog("audioshim: test tone on master\n");
            }
            for (unsigned f = 0; f < n && tleft > 0; f++, tleft--) {
                static const int32_t sine16[16] = { 0, 802, 1482, 1936, 2096, 1936, 1482, 802, 0, -802, -1482, -1936, -2096, -1936, -1482, -802 };
                int32_t v = sine16[(tph++ * 16 * 1000 / (g_rate ? g_rate : 44100)) & 15] * 1000;
                mix[f * 4] += v; mix[f * 4 + 1] += v;
            }
        }
        if (flx) {
            for (unsigned i = 0; i < n * 4; i++) {
                int32_t s = mix[i];
                if (s > 0x7fffff) s = 0x7fffff;
                if (s < -0x800000) s = -0x800000;
                if (flx_fmt == FMT_S24_3LE) { out[i * 3] = s; out[i * 3 + 1] = s >> 8; out[i * 3 + 2] = s >> 16; }
                else ((int16_t *)out)[i] = (int16_t)(s >> 8);
            }
            /* debug: `touch /tmp/audioshim.dump` records the next 2 s sent to the FLX4 */
            {
                static int dfd = -1; static long dleft;
                if (dfd < 0 && syscall(SYS_faccessat, AT_FDCWD, "/tmp/audioshim.dump", 0, 0) == 0) {
                    syscall(SYS_unlinkat, AT_FDCWD, "/tmp/audioshim.dump", 0);
                    dfd = syscall(SYS_openat, AT_FDCWD, "/root/wand/mixdump.raw", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    dleft = (long)(g_rate ? g_rate : 44100) * 2;
                    alog("audioshim: dump start fmt=%d\n", flx_fmt);
                }
                if (dfd >= 0) {
                    syscall(SYS_write, dfd, out, n * 4 * (flx_fmt == FMT_S24_3LE ? 3 : 2));
                    dleft -= n;
                    if (dleft <= 0) { syscall(SYS_close, dfd); dfd = -1; alog("audioshim: dump done\n"); }
                }
            }
            snd_pcm_sframes_t w = real_snd_pcm_writei(flx, out, n);
            if (w < 0) {
                alog("audioshim: FLX4 write %ld (%s)\n", (long)w, real_snd_strerror((int)w));
                if (real_snd_pcm_prepare(flx) < 0) { real_snd_pcm_close(flx); flx = NULL; }
            }
            next = now_us();
        } else {
            /* null sink: keep rbp's audio clock running without the FLX4 */
            next += (int64_t)n * 1000000 / (g_rate ? g_rate : 44100);
            int64_t d = next - now_us();
            if (d > 0) usleep((useconds_t)d); else next = now_us();
        }
    }
    return NULL;
}

static void start_mixer(void)
{
    if (mixer_started) return;
    mixer_started = 1;
    pthread_t t;
    pthread_create(&t, NULL, mixer, NULL);
}

/* ---------------- interposed API ---------------- */
int snd_pcm_open(snd_pcm_t **pcm, const char *name, int stream, int mode)
{
    init_real();
    if (!check_active() || !is_rx3_name(name)) return real_snd_pcm_open(pcm, name, stream, mode);
    pthread_mutex_lock(&mx);
    struct vpcm *v = NULL;
    for (int i = 0; i < MAXV; i++) if (!vdev[i].open) { v = &vdev[i]; break; }
    if (!v) { pthread_mutex_unlock(&mx); return -EBUSY; }
    memset(v, 0, sizeof *v);
    v->magic = VMAGIC; v->open = 1; v->stream = stream;
    snprintf(v->name, sizeof v->name, "%s", name);
    const char *c = strrchr(name, ',');
    v->dev = c ? atoi(c + 1) : 0;
    v->role = stream == STREAM_PLAYBACK ? role_for(v->dev) : ROLE_NONE;
    v->ch = 2; v->rate = 44100; v->fmt = FMT_S24_LE; v->period = 256;
    pthread_mutex_unlock(&mx);
    *pcm = v;
    alog("audioshim: open '%s' stream=%d mode=%d -> virtual dev%d role=%s\n", name, stream, mode, v->dev, role_name[v->role]);
    start_mixer();
    return 0;
}

int snd_pcm_close(snd_pcm_t *pcm)
{
    struct vpcm *v = V(pcm);
    if (!v) return real_snd_pcm_close(pcm);
    alog("audioshim: close '%s'\n", v->name);
    pthread_mutex_lock(&mx); v->open = 0; v->magic = 0; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mx);
    return 0;
}

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *p)
{
    struct vpcm *v = V(pcm);
    if (!v) return real_snd_pcm_hw_params_any(pcm, p);
    ptab_set(p, v);
    return 0;
}
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, int a)
{ return V(pcm) ? 0 : real_snd_pcm_hw_params_set_access(pcm, p, a); }
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, int f)
{
    struct vpcm *v = V(pcm);
    if (!v) return real_snd_pcm_hw_params_set_format(pcm, p, f);
    if (f != FMT_S16_LE && f != FMT_S24_LE && f != FMT_S32_LE && f != FMT_S24_3LE) return -EINVAL;
    v->fmt = f;
    return 0;
}
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned ch)
{
    struct vpcm *v = V(pcm);
    if (!v) return real_snd_pcm_hw_params_set_channels(pcm, p, ch);
    if (ch < 1 || ch > 8) return -EINVAL;
    v->ch = ch;
    return 0;
}
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned *r, int *d)
{
    struct vpcm *v = V(pcm);
    if (!v) return real_snd_pcm_hw_params_set_rate_near(pcm, p, r, d);
    if (*r != 44100 && *r != 48000) *r = 44100;     /* FLX4 does 44.1/48 kHz */
    if (g_rate && *r != g_rate) *r = g_rate;        /* one clock for all streams */
    v->rate = *r;
    return 0;
}
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_uframes_t *s, int *d)
{
    struct vpcm *v = V(pcm);
    if (!v) return real_snd_pcm_hw_params_set_period_size_near(pcm, p, s, d);
    if (*s < 32) *s = 32;
    if (*s > 2048) *s = 2048;
    v->period = *s;
    return 0;
}
int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned *n, int *d)
{ return V(pcm) ? 0 : real_snd_pcm_hw_params_set_periods_near(pcm, p, n, d); }
int snd_pcm_hw_params_test_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned r, int d)
{ return V(pcm) ? ((r == 44100 || r == 48000) ? 0 : -EINVAL) : real_snd_pcm_hw_params_test_rate(pcm, p, r, d); }
int snd_pcm_hw_params_get_channels_min(const snd_pcm_hw_params_t *p, unsigned *v)
{ if (ptab_get(p)) { *v = 1; return 0; } return real_snd_pcm_hw_params_get_channels_min(p, v); }
int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *p, unsigned *v)
{ if (ptab_get(p)) { *v = 2; return 0; } return real_snd_pcm_hw_params_get_channels_max(p, v); }
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *p)
{
    struct vpcm *v = V(pcm);
    if (!v) return real_snd_pcm_hw_params(pcm, p);
    if (!g_rate) g_rate = v->rate;
    alog("audioshim: hw_params '%s' stream=%d ch=%u rate=%u fmt=%d period=%lu\n", v->name, v->stream, v->ch, v->rate, v->fmt, v->period);
    return 0;
}
int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *p)
{ struct vpcm *v = V(pcm); if (!v) return real_snd_pcm_sw_params_current(pcm, p); ptab_set(p, v); return 0; }
int snd_pcm_sw_params_get_boundary(const snd_pcm_sw_params_t *p, snd_pcm_uframes_t *b)
{ if (ptab_get(p)) { *b = 0x40000000; return 0; } return real_snd_pcm_sw_params_get_boundary(p, b); }
int snd_pcm_sw_params_set_silence_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *p, snd_pcm_uframes_t v)
{ return V(pcm) ? 0 : real_snd_pcm_sw_params_set_silence_threshold(pcm, p, v); }
int snd_pcm_sw_params_set_silence_size(snd_pcm_t *pcm, snd_pcm_sw_params_t *p, snd_pcm_uframes_t v)
{ return V(pcm) ? 0 : real_snd_pcm_sw_params_set_silence_size(pcm, p, v); }
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *p, snd_pcm_uframes_t v)
{ return V(pcm) ? 0 : real_snd_pcm_sw_params_set_start_threshold(pcm, p, v); }
int snd_pcm_sw_params_set_stop_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *p, snd_pcm_uframes_t v)
{ return V(pcm) ? 0 : real_snd_pcm_sw_params_set_stop_threshold(pcm, p, v); }
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *p)
{ return V(pcm) ? 0 : real_snd_pcm_sw_params(pcm, p); }
int snd_pcm_prepare(snd_pcm_t *pcm)
{ return V(pcm) ? 0 : real_snd_pcm_prepare(pcm); }
int snd_pcm_link(snd_pcm_t *a, snd_pcm_t *b)
{ return (V(a) || V(b)) ? 0 : real_snd_pcm_link(a, b); }

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t n)
{
    struct vpcm *v = V(pcm);
    if (!v) return real_snd_pcm_writei(pcm, buf, n);
    if (n > RING / 2) n = RING / 2;
    pthread_mutex_lock(&mx);
    /* block like hardware: wait until the mixer has drained room for n frames */
    while (v->open && RING - (v->wr - v->rd) < n) { struct ts32 t; deadline_50ms(&t); cond_timedwait32(&cv, &mx, &t); }
    for (snd_pcm_uframes_t f = 0; f < n; f++) {
        unsigned k = ((v->wr + f) & (RING - 1)) * 2;
        v->ring[k] = to_s24(buf, v->fmt, f * v->ch);
        v->ring[k + 1] = v->ch > 1 ? to_s24(buf, v->fmt, f * v->ch + 1) : v->ring[k];
    }
    v->wr += n; v->frames += n;
    pthread_mutex_unlock(&mx);
    return (snd_pcm_sframes_t)n;
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t n)
{
    struct vpcm *v = V(pcm);
    if (!v) return real_snd_pcm_readi(pcm, buf, n);
    /* RX3 line/mic inputs don't exist here: deliver silence, paced by the mixer's
     * (FLX4) frame clock so rbp's duplex read+write threads run at exactly real time. */
    unsigned bps = v->fmt == FMT_S16_LE ? 2 : v->fmt == FMT_S24_3LE ? 3 : 4;
    memset(buf, 0, n * v->ch * bps);
    pthread_mutex_lock(&mx);
    if (!v->frames) v->rd = g_clock;                        /* first read starts at "now" */
    if ((int)(g_clock - v->rd) > RING) v->rd = g_clock - n; /* fell far behind: resync */
    while (v->open && (int)(g_clock - v->rd) < (int)n) { struct ts32 t; deadline_50ms(&t); cond_timedwait32(&cv, &mx, &t); }
    v->rd += n; v->frames += n;
    pthread_mutex_unlock(&mx);
    return (snd_pcm_sframes_t)n;
}

/* rbp opens a control handle on the RX3 card ("hw:cs4344audiorev8") for pcm info */
static int fake_ctl;
int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode)
{
    init_real();
    if (check_active() && is_rx3_name(name)) { *ctl = &fake_ctl; alog("audioshim: ctl open '%s' (virtual)\n", name); return 0; }
    return real_snd_ctl_open(ctl, name, mode);
}
int snd_ctl_close(snd_ctl_t *ctl) { return ctl == &fake_ctl ? 0 : real_snd_ctl_close(ctl); }
int snd_ctl_pcm_info(snd_ctl_t *ctl, snd_pcm_info_t *info) { return ctl == &fake_ctl ? 0 : real_snd_ctl_pcm_info(ctl, info); }
