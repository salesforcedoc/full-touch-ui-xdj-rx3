/*
 * knobshim-flx4.c — LD_PRELOAD shim: Pioneer DDJ-FLX4 -> XDJ-RX3 rbp keys.
 *
 * Reads the FLX4's USB-MIDI port directly as ALSA rawmidi (/dev/snd/midiCxD0;
 * the Wandboard kernel has no snd-seq, so the sequencer path of knobshim2
 * cannot be used). Card is found by /proc/asound/cardN/id == "DDJFLX4".
 *
 * Target rbp: XDJ-RX3 v1.20, Pioneer's own (md5 4f2efcfc0c9e3f539289f863acfddcc6); the PrimeBox-patched build
 * (3706c68f7242779d46afa09f35a39acf) has the same addresses
 * (sendKey/KeyManager addresses below are specific to that binary).
 *
 * FLX4 MIDI map: verified live on this unit for PLAY/CUE (90 0B/0C), jog
 * (B0 21), browse turn (B6 40) + push (96 41), LOAD (96 46/47), crossfader
 * (B6 1F/3F), ch1 fader (B0 13/33). The rest comes from Mixxx's
 * Pioneer-DDJ-FLX4.midi.xml, which matched every captured message.
 *
 * Env:
 *   FLX_VERBOSE=1   log every MIDI message + mapping to /tmp/knobshim-flx4.log
 *   FLX_DEV=path    force the rawmidi device
 *   JOG_PPR=n       FLX4 jog ticks per revolution (default 720: measured 747 by hand on one turn;
 *                   Mixxx uses 720 for the DDJ-400 family)
 *   JOG_REV=1       reverse jog direction
 *   JOG_IDLE_MS=n   idle ms before speed-0 jog key (default 120)
 *   TEMPO_REV=1     invert tempo fader direction
 *   KNOB_SCALE=n    browse ticks per encoder step (default 1)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>
#include <sys/syscall.h>

static int real_open(const char *p, int flags)
{ return syscall(SYS_openat, AT_FDCWD, p, flags, 0644); }
static ssize_t real_read(int fd, void *b, size_t n) { return syscall(SYS_read, fd, b, n); }
static ssize_t real_write(int fd, const void *b, size_t n) { return syscall(SYS_write, fd, b, n); }
static int real_close(int fd) { return syscall(SYS_close, fd); }

/* ---------------- logging ---------------- */
#define LOG_PATH "/tmp/knobshim-flx4.log"
static int verbose;
static void klog(const char *fmt, ...)
{
     char buf[256];
     va_list ap;
     va_start(ap, fmt);
     int n = vsnprintf(buf, sizeof(buf), fmt, ap);
     va_end(ap);
     if (n <= 0) return;
     if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
     int fd = real_open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
     if (fd < 0) return;
     /* /tmp is a 512 KB RAM disk on the RX3 image: never let the (verbose) log fill it */
     if (syscall(SYS_lseek, fd, 0, SEEK_END) > 128 * 1024) syscall(SYS_ftruncate, fd, 0);
     real_write(fd, buf, n); real_close(fd);
}

/* ---------------- rbp key injection (v1.20 patched rbp) ---------------- */
#include "rbp_process.h"
static int is_rbp_process(void) { return rbp_process_check(); }

#define UI_OBJ_MGR_GLOBAL   0x2685f2cUL
#define KEY_MANAGER_OFF     100
#define SENDKEY_VTABLE_WORD 2
#define OP_PRESS   0
#define OP_RELEASE 2
#define OP_ROTATE  4
#define OP_VALUE   5
#define CH_GLOBAL  1

/* RX3 keycodes (PrimeBox MAPPING.md, live-verified on RX3 v1.20 rbp) */
#define K_SELECTOR     0x420c
#define K_BROWSE       0x0202
#define K_SOURCE       0x0201
#define K_USB1         0x0209
#define K_TAGLIST      0x0203
#define K_MENU         0x0206
#define K_BACK         0x420d
#define K_LOAD         0x4311
#define K_PLAY         0x4101
#define K_CUE          0x4102
#define K_SYNC         0x4112
#define K_JOG_TOUCH    0x4306
#define K_JOG_ROT      0x4305
#define K_TEMPO_RANGE  0x4107
#define K_MT           0x4108
#define K_TEMPO_SLIDER 0x4109
#define K_HOTCUE       0x4113
#define K_ALOOP        0x4114
#define K_SLIPLOOP     0x4115
#define K_BEATJUMP     0x4116
#define K_PAD1         0x4117
#define K_LOOPIN       0x410c
#define K_LOOPOUT      0x410d
#define K_RELOOP       0x410e
#define K_TRIM         0x5019
#define K_EQH          0x501a
#define K_EQM          0x501b
#define K_EQL          0x501c
#define K_FADER        0x501e
#define K_HPCUE        0x5020
#define K_XFADER       0x6017
#define K_HPMIX        0x4405
#define K_XFCURVE      0x6018   /* -> 0x8002 -> XFaderInnards::onEv_FaderCurveSwitched: 0=THRU, 1..3 = curve + ch1->A ch2->B */
#define K_DECKLINESW   0x501f   /* -> 0x1006 -> MixerChInnards::onEv_DeckLineSW: 0=DECK (runs routing()), 1/2=external */
#define K_MASTERCUE    0x4407   /* -> 0x200c -> MasterInnards::onEv_MasterChHeadphoneCue (toggle) */
#define K_MASTERLEVEL  0x4403   /* Mixer::asEventCode -> 0x2003 -> MasterInnards::onEv_MasterLevel */
#define K_COLOR        0x509d
#define K_FILTER       0x50a6
/* RX3 Sound Color FX buttons in panel order (0x50a1..0x50a6, one group in PcControlKeyServer::updateKey):
 * SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER */
#define K_CFX_FIRST    0x50a1
#define CFX_TYPES      6
#define K_BFXTYPE      0x448b
#define K_BFXCH        0x448c
#define K_BFX          0x448d
#define K_DEPTH        0x448f
#define K_BEATPREV     0x4490
#define K_BEATNEXT     0x4491
#define K_HPLEVEL      0x4406   /* headphone LEVEL (FLX4 does it in hardware; rbp pinned to max) */
#define K_CALLFWD      0x4322   /* CUE/LOOP CALL > (halves/doubles a running loop on the RX3) */
#define K_CALLREV      0x4323   /* CUE/LOOP CALL < */
#define K_SHIFT        0x4103   /* RX3 SHIFT button (per deck) */
#define K_DELETE       0x4124   /* memory cue/loop DELETE (RX3 button; FLX4: SHIFT + CUE/LOOP CALL <) */
#define K_MEMORY       0x4125   /* MEMORY (RX3 button; FLX4: SHIFT + CUE/LOOP CALL >) */
/* Beat FX channel selector values (probed on screen): 0 = CH1, 1 = CH2, 2 = MIC, 3 = CF.A, 4 = CF.B,
 * 5 = MASTER, 6 = AUX */
/* Beat FX type = RX3 14-position selector (probed on screen): 0 DELAY, 1 ECHO, 2 PING PONG, 3 SPIRAL,
 * 4 HELIX, 5 REVERB, 6 FLANGER, 7 PHASER, 8 FILTER, 9 TRANS, 10 ROLL, 11 SLIP ROLL, 12 PITCH, 13 VINYL BRAKE */
#define BFX_TYPES      14
#define BFX_CH_1       0
#define BFX_CH_2       1
#define BFX_CH_MASTER  5

static int rbp_ok = -1;
static void *get_key_manager(void)
{
     if (rbp_ok < 0) rbp_ok = is_rbp_process();
     if (!rbp_ok) return NULL;
     void *mgr = *(void **)UI_OBJ_MGR_GLOBAL;
     if (!mgr) return NULL;
     return *(void **)((char *)mgr + KEY_MANAGER_OFF);
}
typedef void (*sendkey_fn)(void *, int, int, int, long, float, long);
static pthread_mutex_t key_lock = PTHREAD_MUTEX_INITIALIZER;
static void send_key_fl(int key, int op, int ch, long param, float f, long l)
{
     void *km = get_key_manager();
     if (!km) return;
     sendkey_fn fn = (sendkey_fn)(*(void ***)km)[SENDKEY_VTABLE_WORD];
     if (!fn) return;
     pthread_mutex_lock(&key_lock);
     fn(km, key, op, ch, param, f, l);
     pthread_mutex_unlock(&key_lock);
}
static void send_key(int key, int op, int ch, long param) { send_key_fl(key, op, ch, param, 0.0f, 0); }
/* exported for uishim-rx3 (on-screen top-row buttons): same KeyManager path and lock as the FLX4 */
void knobshim_send_key(int key, int op, int ch, long param) { send_key(key, op, ch, param); }
/* Sound Color FX PARAMETER (the RX3's one PARAMETER knob): key 0x50a7 -> mixer event 0x100e ->
 * MixerChInnards::onEv_ColorFxParameter(float = IKeyInput +16) -> DjEngineIF::setSoundColorFxParameter(input, f)
 * (per channel; sent to both, like the single RX3 knob). 0.0 .. 1.0. uishim draws / sets it. */
#define K_CFX_PARAM 0x50a7
static volatile float cfx_param = 0.5f;
void knobshim_cfx_param_set(float v)
{
     if (v < 0) v = 0;
     if (v > 1) v = 1;
     cfx_param = v;
     for (int d = 1; d <= 2; d++) send_key_fl(K_CFX_PARAM, OP_VALUE, d, (long)(v * 1023), v, 0);
}
float knobshim_cfx_param(void) { return cfx_param; }
/* Loaded rekordbox track ID per deck (0x8000xxxx; deck 2 = deck 1 + 0x124). Found by diffing rbp memory across
 * loads. The startup DeckLineSW=DECK is sometimes lost (ch2 then plays deck 1), so whenever a deck's track changes,
 * by any load path (LOAD button, browse menu, touch, restore at start), the inject thread re-runs that channel's
 * DECK routing ~1.5 s later: EXTERNAL then DECK forces MixerChInnards::routing() even if rbp already holds DECK. */
#define DECK_TRACK_ID(d) ((volatile uint32_t *)(0x03251cd8UL + ((d) - 1) * 0x124UL))

/* ---------------- mapping tables ---------------- */
/* note map: MIDI channel (0-based) + note -> RX3 key on RX3 channel sch */
#define NNOTES 320
static struct { int ch, note, key, sch, down; } nmap[NNOTES];
static int nmap_n;
/* FLX4 MASTER CUE state (a toggle, off at FLX4 power-up and at rbp start). rbp's own MasterCue key does not put
 * the master into its cue feed (the RX3 hardware does that), so audioshim adds it while this is on. */
extern int audioshim_hp_split(void) __attribute__((weak));
static volatile int master_cue, mc_led_due = 1;   /* re-light the FLX4 MASTER CUE LED (keeps its master path live) */
int knobshim_master_cue(void) { return master_cue; }

static void add_note(int ch, int note, int key, int sch)
{
     if (nmap_n >= NNOTES) return;
     nmap[nmap_n].ch = ch; nmap[nmap_n].note = note;
     nmap[nmap_n].key = key; nmap[nmap_n].sch = sch; nmap[nmap_n].down = 0;
     nmap_n++;
}
/* absolute knobs/faders: MSB CC only (7-bit is plenty for rbp's 10-bit keys) */
#define NABS 24
static struct { int ch, cc, key, sch, last, to_tgt, to_eng, to_last; } amap[NABS];

/* ---------------- soft takeover ----------------
 * rbp starts with its own values (centre for tempo/TRIM/EQ/CFX) and the FLX4 cannot report where its
 * controls physically are, so the first touch of a control that is elsewhere would jump rbp's value.
 * Like Mixxx's soft takeover, such a control only starts acting once its position reaches rbp's
 * value (within TO_TOL of 1023) or crosses it between two messages; after that it passes through.
 * Faders and the crossfader are not covered (rbp starts them at 0 = silent, the first move must act).
 * FLX_NO_TAKEOVER=1 disables. */
#define TO_TOL 16
static int takeover_on = 1;
static int takeover(int *tgt, int *eng, int *last, int v)   /* 1 = act on v */
{
     if (!takeover_on || *eng) { *tgt = v; return 1; }
     int d = v - *tgt, crossed = *last >= 0 && (*last - *tgt) * d <= 0;
     *last = v;
     if (crossed || (d < 0 ? -d : d) <= TO_TOL) { *eng = 1; *tgt = v; return 1; }
     return 0;
}
static int amap_n;
static void add_abs(int ch, int cc, int key, int sch)
{
     if (amap_n >= NABS) return;
     amap[amap_n].ch = ch; amap[amap_n].cc = cc; amap[amap_n].key = key;
     amap[amap_n].sch = sch; amap[amap_n].last = -1;
     /* takeover for the centre-start knobs; others act on first touch */
     int to = key == K_TRIM || key == K_EQH || key == K_EQM || key == K_EQL || key == K_COLOR;
     amap[amap_n].to_tgt = 512; amap[amap_n].to_eng = !to; amap[amap_n].to_last = -1;
     amap_n++;
}

static void build_maps(void)
{
     for (int d = 0; d < 2; d++) {
          int ch = d, sch = 1 + d;           /* deck MIDI ch 0/1 -> RX3 deck 1/2 */
          add_note(ch, 0x0B, K_PLAY, sch);
          add_note(ch, 0x0C, K_CUE, sch);
          add_note(ch, 0x10, K_LOOPIN, sch);
          add_note(ch, 0x11, K_LOOPOUT, sch);
          /* 0x4D 4 BEAT/EXIT: special-cased in handle_note (needs the loop state) */
          add_note(ch, 0x50, K_RELOOP, sch);   /* shift + 4 BEAT/EXIT = RELOOP */
          add_note(ch, 0x51, K_CALLREV, sch);  /* CUE/LOOP CALL < */
          add_note(ch, 0x53, K_CALLFWD, sch);  /* CUE/LOOP CALL > */
          add_note(ch, 0x3E, K_DELETE, sch);   /* shift + CUE/LOOP CALL < = DELETE */
          add_note(ch, 0x3D, K_MEMORY, sch);   /* shift + CUE/LOOP CALL > = MEMORY */
          add_note(ch, 0x58, K_SYNC, sch);
          add_note(ch, 0x36, K_JOG_TOUCH, sch);
          add_note(ch, 0x67, K_JOG_TOUCH, sch);   /* shifted jog touch */
          add_note(ch, 0x1B, K_HOTCUE, sch);      /* pad mode: HOT CUE */
          add_note(ch, 0x1E, K_SLIPLOOP, sch);    /* pad mode: PAD FX1 */
          add_note(ch, 0x20, K_BEATJUMP, sch);    /* pad mode: BEAT JUMP */
          add_note(ch, 0x22, K_ALOOP, sch);       /* pad mode: SAMPLER -> auto loop */
          /* headphone CUE -> RX3 mixer HeadphoneCueCh (0x5020, from rbp's
           * PcControlKeyData::keyCode2Text table) */
          add_note(ch, 0x54, K_HPCUE, sch);
          add_note(ch, 0x3F, 0, sch);             /* SHIFT — log only */
          /* performance pads: ch 7 (deck1) / 9 (deck2); every pad-mode bank
           * 0x00..0x77 -> RX3 pad 1..8 (RX3 applies its own current pad mode) */
          for (int bank = 0; bank < 8; bank++)
               for (int p = 0; p < 8; p++)
                    add_note(d ? 9 : 7, bank * 0x10 + p, K_PAD1 + p, sch);
          /* mixer */
          add_abs(ch, 0x04, K_TRIM, sch);
          add_abs(ch, 0x07, K_EQH, sch);
          add_abs(ch, 0x0B, K_EQM, sch);
          add_abs(ch, 0x0F, K_EQL, sch);
          add_abs(ch, 0x13, K_FADER, sch);
          add_abs(6, 0x17 + d, K_COLOR, sch);     /* CFX knobs */
     }
     /* global (MIDI ch 6) */
     add_note(6, 0x41, K_SELECTOR, CH_GLOBAL);   /* browse push */
     add_note(6, 0x42, K_BACK, CH_GLOBAL);       /* shift + browse push: RX3 BACK (BROWSE is on screen) */
     /* shift + LOAD deck 1 (96 68) = RX3 MENU/UTILITY: special-cased in handle_note (tap / 2 s hold) */
     add_note(6, 0x46, K_LOAD, 1);
     add_note(6, 0x47, K_LOAD, 2);
     add_abs(6, 0x1F, K_XFADER, CH_GLOBAL);
     /* HEADPHONE MIX (B6 0C) is applied by the FLX4 in hardware (cue = USB 3/4, master = USB 1/2
      * while the FLX4's own MASTER CUE is lit), so it is NOT forwarded; rbp's mix is pinned to
      * CUE at start. */
     /* MASTER CUE (96 63) = RX3 headphone CUE (LINK) / MasterCue 0x4407 (toggle), forwarded so rbp's state matches.
      * rbp does not mix the master into its cue feed itself: audioshim does (knobshim_master_cue()).
      * The FLX4's hardware master path (the MASTER side of HEADPHONE MIX) follows its MASTER CUE *LED*, which the
      * host sets (96 63 7F): output_thread keeps it lit, so HEADPHONE MIX works like the RX3's (MASTER side = master,
      * always) and the button only toggles the RX3 function; its state is shown on screen (uishim). */
     add_note(6, 0x63, K_MASTERCUE, CH_GLOBAL);
     /* Beat FX (MIDI ch 4) */
     add_note(4, 0x47, K_BFX, CH_GLOBAL);        /* FX ON/OFF, lever at CH1 or CH1&CH2 */
     add_note(5, 0x47, K_BFX, CH_GLOBAL);        /* FX ON/OFF, lever at CH2 (sent on MIDI ch 6) */
     add_note(4, 0x4A, K_BEATPREV, CH_GLOBAL);
     add_note(4, 0x4B, K_BEATNEXT, CH_GLOBAL);
     /* FX SELECT 94 63 / SHIFT 94 64: special-cased in handle_note (steps the 14-position selector) */
     add_abs(4, 0x02, K_DEPTH, CH_GLOBAL);
}

/* USB1 mount point: Pioneer's udev rule uses /media/usb1/<kernel name> (sda1, sdb1, ... per boot) */
static void usb1_dir(char *out, size_t n)
{
     FILE *f = fopen("/proc/mounts", "r");
     char line[256], mnt[96];
     snprintf(out, n, "/media/usb1/sda1");
     if (!f) return;
     while (fgets(line, sizeof line, f))
          if (sscanf(line, "%*s %95s", mnt) == 1 && strncmp(mnt, "/media/usb1/", 12) == 0) { snprintf(out, n, "%s", mnt); break; }
     fclose(f);
}
static int usb1_file(const char *rel)
{
     char d[96], p[192];
     usb1_dir(d, sizeof d);
     snprintf(p, sizeof p, "%s/%s", d, rel);
     return access(p, F_OK) == 0;
}

/* ---------------- USB1 remap (browse push in Source menu) ---------------- */
static int source_menu_with_usb1(void)
{
     /* stick registered by rbp but no browse device chosen yet -> the
      * browse push should open USB1 (the FLX4 has no USB/source button) */
     if (*(volatile uint32_t *)0x03256888 == 2 && *(volatile uint32_t *)0x326f8bc == 0)
          return 1;
     if (*(volatile uint32_t *)0x326f8b8 != 12) return 0;   /* browseMode */
     return usb1_file("PIONEER/rekordbox/export.pdb") || usb1_file("PIONEER/rekordbox/exportExt.pdb");
}

/* ---------------- 4 BEAT / EXIT ----------------
 * RX3: holding LOOP IN ~1 s sets a 4-beat loop; RELOOP/EXIT leaves a running loop. The FLX4's
 * 4 BEAT/EXIT does "exit if looping, else 4-beat loop", so it needs the deck's loop state: read the
 * shown flag of the deck screen's loop icon (DECK component, same object-ID route as uishim). */
#define WS_BROWSER_VFT 0x004c8290u
typedef void *(*get_mgr_fn)(void);
typedef void *(*get_active_fn)(void *);
typedef void *(*get_obj_fn)(void *, int);
static const int loop_icon_id[2] = { 942, 981 };   /* DECK_1/2 GRP_LOOP_IMG_LOOPICON (global IDs) */
static int loop_active(int deck)                   /* 1 / 0, -1 = unknown */
{
     void *mgr = ((get_mgr_fn)0x001d0a30)();                       /* Get_NS_ComponentManager */
     if (!mgr) return -1;
     uint32_t *ws = ((get_active_fn)0x001cc510)(mgr);               /* ..._GetActiveWinscape */
     if (!ws || ws[0] != WS_BROWSER_VFT) return -1;
     uint32_t *o = ((get_obj_fn)0x0018dae8)(0, loop_icon_id[deck]); /* ui_com_draw_GetObjectByID */
     /* the icon is shown whenever the deck has a loop (also a saved one); its image is 0x14e7
      * (orange) only while the loop plays, 0x14e6 otherwise */
     return o ? (o[3] != 0 && o[17] == 0x14e7) : -1;
}
/* 4-beat loop like the RX3 panel's held LOOP IN/4 BEAT: press, then the long-press operation (op 1:
 * PlayerInnards::onKey_LoopIn -> DjEngineIF::setAutoBeatLoop), then release. A plain 1 s hold of
 * PRESS only set a loop-in point. */
#define OP_LONGPRESS 1
/* LOOP IN/4 BEAT follows the physical button: PRESS on press, the long-press operation once it has been
 * held 1 s (op 1: PlayerInnards::onKey_LoopIn -> DjEngineIF::setAutoBeatLoop = 4-beat loop), RELEASE on
 * release. A tap therefore sets a loop-in point, like tapping LOOP IN on the RX3. */
static volatile int loopin_gen[2], loopin_held[2];
static void *loopin_longpress(void *arg)
{
     int d = (int)(intptr_t)arg & 1, gen = (int)(intptr_t)arg >> 1;
     usleep(1000000);
     if (loopin_held[d] && loopin_gen[d] == gen) {
          send_key(K_LOOPIN, OP_LONGPRESS, d + 1, 0);
          if (verbose) klog("flx4: 4BEAT deck%d held 1 s -> 4-beat loop\n", d + 1);
     }
     return NULL;
}

/* MENU/UTILITY (SHIFT + LOAD 1): PRESS on press, the long-press operation after 2 s held (= UTILITY, like
 * holding the RX3's MENU/UTILITY button; same hold as the on-screen MENU), RELEASE on release. */
static volatile int menu_gen, menu_held;
static void *menu_longpress(void *arg)
{
     int gen = (int)(intptr_t)arg;
     usleep(2000000);
     if (menu_held && menu_gen == gen) {
          send_key(K_MENU, OP_LONGPRESS, CH_GLOBAL, 0);
          if (verbose) klog("flx4: MENU held 2 s -> UTILITY\n");
     }
     return NULL;
}

/* current Sound Color FX type (0 SPACE .. 5 FILTER); exported for uishim-rx3's deck-box label */
static int cfx = CFX_TYPES - 1;                         /* FILTER, selected at start */
int knobshim_cfx(void) { return cfx; }
/* CFX knob per deck (0..1023, 512 = centre/off; -1 = not moved yet); exported for uishim's bar */
static volatile int cfx_knob[2] = { -1, -1 };
int knobshim_cfx_knob(int deck) { return deck >= 0 && deck < 2 ? cfx_knob[deck] : -1; }

/* rbp's MENU window (MY SETTINGS popup: LOAD / BACKGROUND COLOR / WAVEFORM COLOR) is operated with the
 * rotary selector (it ignores touch); global object 540 in WS_BROWSER, shown flag +12. */
static int menu_popup_open(void)
{
     void *mgr = ((get_mgr_fn)0x001d0a30)();
     uint32_t *ws = mgr ? ((get_active_fn)0x001cc510)(mgr) : NULL;
     if (!ws || ws[0] != WS_BROWSER_VFT) return 0;
     uint32_t *o = ((get_obj_fn)0x0018dae8)(0, 540);
     return o && o[3];
}

/* FX CH SELECT lever: position = (94 10, 95 11) -> CH1 (1,0), CH2 (0,1), CH1&CH2 (1,1) */
static int fx_lever_a, fx_lever_b;

/* SHIFT layer of the pad section, handled by rbp natively: the FLX4 sends distinct notes for
 * SHIFT + pad-mode button (69/6B/6D/6F) and SHIFT + pad (MIDI ch 9/11 = 0x98/0x9A, same bank layout);
 * send the RX3 key with the RX3 SHIFT held around it, so rbp applies its own second function. */
static int shifted_pad_key(int ch, int note, int *key, int *sch)
{
     if (ch == 0 || ch == 1) {
          *sch = ch + 1;
          switch (note) {
          case 0x69: *key = K_HOTCUE;   return 1;   /* SHIFT + HOT CUE mode */
          case 0x6B: *key = K_SLIPLOOP; return 1;   /* SHIFT + PAD FX 1 mode */
          case 0x6D: *key = K_BEATJUMP; return 1;   /* SHIFT + BEAT JUMP mode */
          case 0x6F: *key = K_ALOOP;    return 1;   /* SHIFT + SAMPLER mode */
          }
          return 0;
     }
     if ((ch == 8 || ch == 10) && (note & 0x0f) < 8 && note < 0x80) {
          *sch = ch == 8 ? 1 : 2;
          *key = K_PAD1 + (note & 0x0f);              /* any bank -> RX3 pad 1..8 */
          return 1;
     }
     return 0;
}

static void handle_note(int ch, int note, int on)
{
     /* FLX4 SHIFT + BEAT SYNC = INST. DOUBLES: on the RX3 that is a long press of BEAT SYNC/INST.DOUBLES (manual
      * p. 74), so send SYNC press + long press, then the release (tempo range is on the touch screen now) */
     if ((ch == 0 || ch == 1) && (note == 0x5C || note == 0x60)) {   /* 0x5C = SHIFT + SYNC (seen live), 0x60 = SYNC held */
          if (on) { send_key(K_SYNC, OP_PRESS, ch + 1, 0); send_key(K_SYNC, 1 /* long press */, ch + 1, 0); }
          else send_key(K_SYNC, OP_RELEASE, ch + 1, 0);
          if (verbose) klog("flx4: SHIFT+SYNC deck %d -> INST. DOUBLES %s\n", ch + 1, on ? "press" : "release");
          return;
     }
     {
          int key, sch;
          if (shifted_pad_key(ch, note, &key, &sch)) {
               if (on) { send_key(K_SHIFT, OP_PRESS, sch, 0); send_key(key, OP_PRESS, sch, 0); }
               else { send_key(key, OP_RELEASE, sch, 0); send_key(K_SHIFT, OP_RELEASE, sch, 0); }
               if (verbose) klog("flx4: SHIFT+ch%d note%02x -> SHIFT+0x%04x %s sch%d\n", ch, note, key, on ? "press" : "release", sch);
               return;
          }
     }
     if ((ch == 4 && note == 0x10) || (ch == 5 && note == 0x11)) {
          if (ch == 4) fx_lever_a = on; else fx_lever_b = on;
          int sel = fx_lever_a && fx_lever_b ? BFX_CH_MASTER : fx_lever_a ? BFX_CH_1 : fx_lever_b ? BFX_CH_2 : -1;
          if (sel >= 0) {
               send_key(K_BFXCH, OP_VALUE, CH_GLOBAL, sel);
               if (verbose) klog("flx4: FX CH lever -> selector %d\n", sel);
          }
          return;
     }
     if (ch == 6 && note == 0x68) {                     /* SHIFT + LOAD 1 = MENU (hold 2 s = UTILITY) */
          if (on && !menu_held) {
               int gen = ++menu_gen;
               menu_held = 1;
               send_key(K_MENU, OP_PRESS, CH_GLOBAL, 0);
               pthread_t t;
               if (pthread_create(&t, NULL, menu_longpress, (void *)(intptr_t)gen) == 0) pthread_detach(t);
          } else if (!on && menu_held) {
               menu_held = 0;
               send_key(K_MENU, OP_RELEASE, CH_GLOBAL, 0);
          }
          return;
     }
     if (ch == 6 && (note == 0x00 || note == 0x08)) {   /* SMART CFX (+SHIFT = back): Sound Color FX type */
          if (!on) return;
          cfx = (cfx + (note == 0x00 ? 1 : CFX_TYPES - 1)) % CFX_TYPES;
          for (int d = 1; d <= 2; d++) {                    /* like the startup FILTER press */
               send_key(K_CFX_FIRST + cfx, OP_PRESS, d, 0);
               send_key(K_CFX_FIRST + cfx, OP_RELEASE, d, 0);
          }
          ((void (*)(void))0x0018e214)();                  /* ui_com_draw_RefreshWinscape: uishim shows the name */
          if (verbose) klog("flx4: SMART CFX -> 0x%04x\n", K_CFX_FIRST + cfx);
          return;
     }
     if (ch == 4 && (note == 0x63 || note == 0x64)) {   /* FX SELECT (+SHIFT = back) */
          static int bfx_type;
          if (!on) return;
          bfx_type = (bfx_type + (note == 0x63 ? 1 : BFX_TYPES - 1)) % BFX_TYPES;
          send_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, bfx_type);
          if (verbose) klog("flx4: FX SELECT -> type %d\n", bfx_type);
          return;
     }
     if ((ch == 0 || ch == 1) && note == 0x4D) {
          static int exiting[2];
          int d = ch;
          if (on) {
               int la = loop_active(d);
               exiting[d] = la == 1;
               if (exiting[d]) send_key(K_RELOOP, OP_PRESS, d + 1, 0);
               else {
                    int gen = ++loopin_gen[d];
                    loopin_held[d] = 1;
                    send_key(K_LOOPIN, OP_PRESS, d + 1, 0);
                    pthread_t t;
                    if (pthread_create(&t, NULL, loopin_longpress, (void *)(intptr_t)(gen << 1 | d)) == 0) pthread_detach(t);
               }
               if (verbose) klog("flx4: 4BEAT/EXIT deck%d loop=%d -> %s\n", d + 1, la, exiting[d] ? "EXIT" : "LOOP IN/4 BEAT");
          } else if (exiting[d]) {
               send_key(K_RELOOP, OP_RELEASE, d + 1, 0);
               exiting[d] = 0;
          } else if (loopin_held[d]) {
               loopin_held[d] = 0;
               send_key(K_LOOPIN, OP_RELEASE, d + 1, 0);
          }
          return;
     }
     /* browse push: the RX3 selector only acts inside the browser, so in the
      * deck view (browseMode 1) it must send BROWSE to open the list. The key
      * chosen on press is reused on release. */
     static int push_key;
     if (ch == 6 && note == 0x41) {
          if (on) {
               uint32_t mode = *(volatile uint32_t *)0x326f8b8;
               push_key = menu_popup_open()        ? K_SELECTOR   /* MY SETTINGS menu: select the item */
                        : source_menu_with_usb1() ? K_USB1
                        : (mode == 1)             ? K_BROWSE
                        :                           K_SELECTOR;
          } else if (!push_key) return;
          send_key(push_key, on ? OP_PRESS : OP_RELEASE, push_key == K_SELECTOR ? 1 : CH_GLOBAL, 0);
          if (verbose) klog("flx4: browse push -> 0x%04x %s\n", push_key, on ? "press" : "release");
          if (!on) push_key = 0;
          return;
     }
     if (ch == 6 && note == 0x63) {                 /* the FLX4 toggles its own LED (and master path) on a press */
          mc_led_due = 1;
          if (on) {                                  /* MASTER CUE: our copy of the toggle for audioshim */
               master_cue = !master_cue;
               klog("flx4: master cue %s\n", master_cue ? "on" : "off");
          }
     }
     for (int i = 0; i < nmap_n; i++) {
          if (nmap[i].ch != ch || nmap[i].note != note) continue;
          if (!nmap[i].key) { if (verbose) klog("flx4: ch%d note%02x (log-only) %d\n", ch, note, on); return; }
          if (on && !nmap[i].down) {
               nmap[i].down = 1;
               send_key(nmap[i].key, OP_PRESS, nmap[i].sch, 0);
          } else if (!on && nmap[i].down) {
               nmap[i].down = 0;
               send_key(nmap[i].key, OP_RELEASE, nmap[i].sch, 0);
          } else return;
          if (verbose) klog("flx4: ch%d note%02x -> 0x%04x %s sch%d\n", ch, note,
                            nmap[i].key, on ? "press" : "release", nmap[i].sch);
          return;
     }
     if (verbose) klog("flx4: unmapped ch%d note%02x %d\n", ch, note, on);
}

static int cc_to_10bit(int v) { return (v << 3) | (v >> 4); }

/* ---------------- jog (relative, CC 0x21/0x22/0x23/0x29, centre 0x40) ---------------- */
static struct { unsigned vpos; unsigned long long last_ms; int moving; } jog[2];
static int jog_ppr = 720, jog_rev, jog_idle_ms = 120, tempo_rev, knob_scale = 1;
/* The chroot's glibc 2.13 keeps clock_gettime in librt, not libc, so go to the kernel directly:
 * 263 on ARM EABI is the 32-bit-timespec one. (asm-generic headers spell it SYS_clock_gettime32,
 * the 32-bit ARM ones SYS_clock_gettime -- same number either way. Same idiom as audioshim.) */
#ifndef SYS_clock_gettime
#define SYS_clock_gettime 263                  /* ARM EABI, 32-bit time (kernel 3.0) */
#endif
static unsigned long long now_ms(void)
{
     struct { long tv_sec; long tv_nsec; } ts;
     syscall(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, &ts);
     return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}
static void handle_jog(int d, int val)
{
     int delta = val - 0x40;
     if (!delta) return;
     if (jog_rev) delta = -delta;
     unsigned long long t = now_ms();
     float dt = jog[d].last_ms ? (float)(t - jog[d].last_ms) / 1000.0f : 0.01f;
     if (dt < 0.002f) dt = 0.002f;
     if (dt > 0.1f) dt = 0.1f;
     jog[d].last_ms = t;
     /* rbp's JogPulse takes a u16 counter (wraps at 65536) in RX3 jog pulses: 12.25 samples each, one turn =
      * 1.8 s = 6480 pulses (JogPulse::JOG_MAX_COUNT_), so a FLX4 turn moves the track 1.8 s like the RX3's. */
     int dp = delta * 6480 / jog_ppr;
     if (!dp) dp = delta > 0 ? 1 : -1;
     jog[d].vpos = (jog[d].vpos + (unsigned)dp) & 0xFFFFu;
     /* speed in rbp's unit, 1.0 = one turn per 1.8 s (normal play speed on the RX3's 33 1/3 platter);
      * Player::setJogSpeed treats |speed| < 0.05 as stopped, so rev/s alone made slow turns vanish */
     float speed = (float)delta / (float)jog_ppr / dt * 1.8f;
     if (speed > 8.0f) speed = 8.0f;
     if (speed < -8.0f) speed = -8.0f;
     jog[d].moving = 1;
     send_key_fl(K_JOG_ROT, OP_ROTATE, d + 1, 0, speed, (long)jog[d].vpos);
     if (verbose) klog("flx4: jog d%d delta=%d speed=%.2f pos=%u\n", d + 1, delta, (double)speed, jog[d].vpos);
}
static void *jog_idle_thread(void *arg)
{
     (void)arg;
     for (;;) {
          usleep(30000);
          unsigned long long t = now_ms();
          for (int d = 0; d < 2; d++)
               if (jog[d].moving && t - jog[d].last_ms >= (unsigned long long)jog_idle_ms) {
                    jog[d].moving = 0;
                    send_key_fl(K_JOG_ROT, OP_ROTATE, d + 1, 0, 0.0f, (long)jog[d].vpos);
               }
     }
     return NULL;
}

/* ---------------- tempo (14-bit CC 0x00 MSB / 0x20 LSB) ---------------- */
static int tempo_msb[2];
static void handle_tempo_lsb(int d, int lsb)
{
     int pos = (tempo_msb[d] << 7) | lsb;          /* 0 = top */
     float norm = ((float)pos - 8192.0f) / 8192.0f; /* top -> -1 (slower), bottom -> +1 */
     if (norm > 1.0f) norm = 1.0f;
     if (norm < -1.0f) norm = -1.0f;
     if (tempo_rev) norm = -norm;
     int v10 = (int)((norm + 1.0f) * 511.5f);
     static int t_tgt[2] = { 512, 512 }, t_eng[2], t_last[2] = { -1, -1 };
     if (!t_eng[d]) {
          if (!takeover(&t_tgt[d], &t_eng[d], &t_last[d], v10)) {
               if (verbose) klog("flx4: tempo d%d at %d waiting for takeover (rbp 512 = 0%%)\n", d + 1, v10);
               return;
          }
          klog("flx4: tempo d%d taken over at %d\n", d + 1, v10);
     }
     send_key_fl(K_TEMPO_SLIDER, OP_VALUE, d + 1, v10, norm, pos);
     if (verbose) klog("flx4: tempo d%d pos=%d norm=%.3f\n", d + 1, pos, (double)norm);
}

static void handle_cc(int ch, int cc, int val)
{
     if (ch <= 1 && (cc == 0x21 || cc == 0x22 || cc == 0x23 || cc == 0x29)) { handle_jog(ch, val); return; }
     if (ch <= 1 && cc == 0x00) { tempo_msb[ch] = val; return; }
     if (ch <= 1 && cc == 0x20) { handle_tempo_lsb(ch, val); return; }
     if (ch == 6 && cc == 0x40) {                   /* browse encoder: 01 = +1, 7F = -1 */
          int delta = val >= 64 ? val - 128 : val;
          delta *= knob_scale;
          if (delta > 16) delta = 16;
          if (delta < -16) delta = -16;
          for (int i = 0; i < (delta < 0 ? -delta : delta); i++)
               send_key(K_SELECTOR, OP_ROTATE, CH_GLOBAL, delta < 0 ? -1 : 1);
          if (verbose) klog("flx4: browse delta=%d\n", delta);
          return;
     }
     for (int i = 0; i < amap_n; i++) {
          if (amap[i].ch != ch || amap[i].cc != cc) continue;
          int v = cc_to_10bit(val);
          if (v == amap[i].last) return;
          amap[i].last = v;
          if (!amap[i].to_eng) {
               if (!takeover(&amap[i].to_tgt, &amap[i].to_eng, &amap[i].to_last, v)) {
                    if (verbose) klog("flx4: ch%d cc%02x=%d waiting for takeover (rbp %d)\n", ch, cc, val, amap[i].to_tgt);
                    return;
               }
               klog("flx4: ch%d cc%02x -> 0x%04x taken over at %d\n", ch, cc, amap[i].key, v);
          }
          /* CFX COLOR is bipolar (centre = off) and takes a VALUE; LEVEL/DEPTH is 0..max like
           * the other knobs: sent as VALUE it peaked at the centre. */
          int op = amap[i].key == K_COLOR ? OP_VALUE : OP_ROTATE;
          send_key_fl(amap[i].key, op, amap[i].sch, v, (float)v / 1023.0f, 0);
          if (amap[i].key == K_COLOR && amap[i].sch >= 1 && amap[i].sch <= 2) {
               static unsigned long long last_refresh;
               cfx_knob[amap[i].sch - 1] = v;
               unsigned long long t = now_ms();
               if (t - last_refresh >= 50) {               /* redraw for uishim's CFX bar, <= 20/s */
                    last_refresh = t;
                    ((void (*)(void))0x0018e214)();         /* ui_com_draw_RefreshWinscape */
               }
          }
          if (verbose) klog("flx4: ch%d cc%02x=%d -> 0x%04x v=%d sch%d\n", ch, cc, val, amap[i].key, v, amap[i].sch);
          return;
     }
     /* LSB halves of 14-bit controls are silently ignored */
     if (verbose && !(cc >= 0x20 && cc <= 0x3F)) klog("flx4: unmapped ch%d cc%02x=%d\n", ch, cc, val);
}

/* ---------------- rawmidi device discovery + parser ---------------- */
static int find_flx4_dev(char *out, size_t n)
{
     const char *f = getenv("FLX_DEV");
     if (f) { snprintf(out, n, "%s", f); return 0; }
     for (int c = 0; c < 8; c++) {
          char p[64], id[32] = {0};
          snprintf(p, sizeof(p), "/proc/asound/card%d/id", c);
          int fd = real_open(p, O_RDONLY);
          if (fd < 0) continue;
          ssize_t r = real_read(fd, id, sizeof(id) - 1);
          real_close(fd);
          if (r > 0 && strncmp(id, "DDJFLX4", 7) == 0) {
               snprintf(out, n, "/dev/snd/midiC%dD0", c);
               return 0;
          }
     }
     return -1;
}

static void *midi_thread(void *arg)
{
     (void)arg;
     const char *s;
     verbose = getenv("FLX_VERBOSE") != NULL;
     if ((s = getenv("JOG_PPR")) && atoi(s) > 0) jog_ppr = atoi(s);
     if ((s = getenv("JOG_IDLE_MS")) && atoi(s) >= 10) jog_idle_ms = atoi(s);
     if ((s = getenv("KNOB_SCALE")) && atoi(s) > 0) knob_scale = atoi(s);
     jog_rev = getenv("JOG_REV") != NULL;
     takeover_on = !(getenv("FLX_NO_TAKEOVER") && atoi(getenv("FLX_NO_TAKEOVER")));
     tempo_rev = getenv("TEMPO_REV") != NULL;
     build_maps();
     klog("flx4: thread start (%d/%d notes, %d abs)\n", nmap_n, NNOTES, amap_n);

     for (int i = 0; i < 600 && !get_key_manager(); i++) usleep(100000);
     if (!get_key_manager()) { klog("flx4: KeyManager never ready\n"); return NULL; }
     klog("flx4: KeyManager ready\n");
     send_key(K_BFXCH, OP_VALUE, CH_GLOBAL, BFX_CH_MASTER);
     send_key(K_BFXTYPE, OP_VALUE, CH_GLOBAL, 0);  /* FX SELECT stepping starts from DELAY */
     /* The RX3 reads MASTER LEVEL from its front-panel knob; the FLX4's master knob is
      * analog (no MIDI), so rbp's master level would stay at 0 = silent master. Set it
      * like an RX3 with the knob fully up; the FLX4's own knob then sets the volume. */
     {
          const char *ml = getenv("MASTER_LEVEL");
          int v = ml ? atoi(ml) : 1023;
          if (v < 0) v = 0;
          if (v > 1023) v = 1023;
          send_key_fl(K_MASTERLEVEL, OP_ROTATE, CH_GLOBAL, v, (float)v / 1023.0f, 0);
          klog("flx4: master level -> %d\n", v);
     }
     knobshim_cfx_param_set(0.5f);                  /* CFX PARAMETER: centre, so rbp and the on-screen bar agree */
     /* HEADPHONE LEVEL is left at rbp's default: the FLX4 knob sets the headphone volume in
      * hardware, and pinning rbp's level to max made the cue feed much louder than the master
      * feed in the FLX4's headphone mix. HP_LEVEL=<0..1023> overrides. */
     if (getenv("HP_LEVEL")) {
          int v = atoi(getenv("HP_LEVEL"));
          send_key_fl(K_HPLEVEL, OP_ROTATE, CH_GLOBAL, v, (float)v / 1023.0f, 0);
          klog("flx4: headphone level -> %d\n", v);
     }
     /* The RX3's crossfader curve switch also sets the channel assign; with no panel rbp
      * sits at THRU and ignores the crossfader. FLX4 decks are hard-wired 1=A, 2=B. */
     {
          const char *xc = getenv("XFADER_CURVE");
          int pos = xc ? atoi(xc) : 2;
          if (pos < 0 || pos > 3) pos = 2;
          send_key(K_XFCURVE, OP_VALUE, CH_GLOBAL, pos);
          klog("flx4: crossfader curve switch -> %d\n", pos);
     }
     /* Mixer channel input selectors (RX3 front panel: DECK/external). With no panel rbp never
      * runs MixerChInnards::routing(), so mixer ch2 stayed fed from deck 1 (deck 2 silent on
      * master, ch2 CUE cued deck 1 — the old "both CUEs cue deck 1" bug). Report DECK for both. */
     send_key(K_DECKLINESW, OP_VALUE, 1, 0);
     send_key(K_DECKLINESW, OP_VALUE, 2, 0);
     klog("flx4: mixer ch1/ch2 input -> DECK\n");
     /* The FLX4 mixes the headphones in hardware (like a DJ mixer): rbp must deliver a pure
      * cue feed on its HP bus. Pin rbp's HEADPHONE MIX to the CUE end (0) and leave rbp's
      * MASTER CUE off (MASTER_CUE=1 turns it on, e.g. for FLX4-less use). */
     send_key_fl(K_HPMIX, OP_ROTATE, CH_GLOBAL, 0, 0.0f, 0);
     klog("flx4: rbp headphone mix pinned to CUE\n");
     if (getenv("MASTER_CUE") && atoi(getenv("MASTER_CUE"))) {
          send_key(K_MASTERCUE, OP_PRESS, CH_GLOBAL, 0);
          send_key(K_MASTERCUE, OP_RELEASE, CH_GLOBAL, 0);
          klog("flx4: rbp master cue -> on\n");
     }
     send_key(K_FILTER, OP_PRESS, 1, 0); send_key(K_FILTER, OP_RELEASE, 1, 0);
     send_key(K_FILTER, OP_PRESS, 2, 0); send_key(K_FILTER, OP_RELEASE, 2, 0);

     for (;;) {                                     /* (re)open loop: survives hot-plug */
          char dev[64];
          int fd = -1;
          if (find_flx4_dev(dev, sizeof(dev)) == 0)
               fd = real_open(dev, O_RDONLY);
          if (fd < 0) { sleep(2); continue; }
          klog("flx4: opened %s\n", dev);
          unsigned char st = 0, d1 = 0;
          int need = 0, have = 0;
          for (;;) {
               struct pollfd p = { fd, POLLIN, 0 };
               int pr = poll(&p, 1, 1000);
               if (pr < 0 && errno == EINTR) continue;
               if (pr < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
               if (pr == 0) continue;
               unsigned char buf[256];
               ssize_t n = real_read(fd, buf, sizeof(buf));
               if (n < 0 && errno == EINTR) continue;
               if (n <= 0) break;
               for (ssize_t i = 0; i < n; i++) {
                    unsigned char b = buf[i];
                    if (b >= 0xF8) continue;             /* realtime */
                    if (b & 0x80) {
                         if (b >= 0xF0) { st = 0; continue; } /* sysex/common: ignore */
                         st = b; have = 0;
                         need = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
                         continue;
                    }
                    if (!st) continue;
                    if (have == 0) { d1 = b; have = 1; if (need == 1) { have = 0; } continue; }
                    have = 0;                            /* complete 3-byte msg; keep running status */
                    int ch = st & 0x0F;
                    if (getenv("FLX_RAW_LOG")) klog("flx4: rx %02x %02x %02x\n", st, d1, b);
                    switch (st & 0xF0) {
                    case 0x90: handle_note(ch, d1, b != 0); break;
                    case 0x80: handle_note(ch, d1, 0); break;
                    case 0xB0: handle_cc(ch, d1, b); break;
                    }
               }
          }
          real_close(fd);
          klog("flx4: device closed, waiting for re-plug\n");
          sleep(1);
     }
     return NULL;
}

/* ---------------- remote key injection (debug) ----------------
 * Write "KEY OP CH [PARAM]" lines (hex or dec) to /tmp/flx4-inject inside
 * the chroot; the file is consumed and deleted.  OP: 0 press, 2 release,
 * 4 rotate, 9 = press+release tap. */
/* raw MIDI to the FLX4, queued by the inject file ("midi 96 63 7f") and sent by output_thread */
static unsigned char raw_out[64];
static volatile int raw_n;
#define INJECT_PATH "/tmp/flx4-inject"
static void *inject_thread(void *arg)
{
     (void)arg;
     unsigned long long route_due[3] = { 0, 0, 0 };
     uint32_t track[3] = { 0, 0, 0 };
     for (;;) {
          usleep(100000);
          if (!get_key_manager()) continue;
          for (int c = 1; c <= 2; c++) {
               uint32_t id = *DECK_TRACK_ID(c);
               if (id != track[c]) {
                    track[c] = id;
                    if (id) { route_due[c] = now_ms() + 1500; klog("flx4: deck%d track 0x%08x loaded\n", c, id); }
               }
               if (route_due[c] && now_ms() >= route_due[c]) {
                    route_due[c] = 0;
                    send_key(K_DECKLINESW, OP_VALUE, c, 1);
                    usleep(100000);
                    send_key(K_DECKLINESW, OP_VALUE, c, 0);
                    klog("flx4: mixer ch%d input re-asserted -> DECK (after load)\n", c);
               }
          }
          int fd = real_open(INJECT_PATH, O_RDONLY);
          if (fd < 0) continue;
          char buf[512];
          ssize_t n = real_read(fd, buf, sizeof(buf) - 1);
          real_close(fd);
          syscall(SYS_unlinkat, AT_FDCWD, INJECT_PATH, 0);
          if (n <= 0) continue;
          buf[n] = 0;
          char *line = buf, *nl;
          while (line && *line) {
               nl = strchr(line, '\n');
               if (nl) *nl = 0;
               long key = 0, op = 0, ch = 1, param = 0;
               char *e;
               if (!strncmp(line, "cfxp ", 5)) {        /* debug: "cfxp <0..1>" = Sound Color FX PARAMETER */
                    float v = (float)strtod(line + 5, NULL);
                    knobshim_cfx_param_set(v);
                    klog("flx4: CFX PARAMETER -> %.2f\n", (double)v);
                    line = nl ? nl + 1 : NULL;
                    continue;
               }
               if (!strncmp(line, "midi ", 5)) {        /* debug: "midi <hex bytes>" = raw MIDI to the FLX4 */
                    int n = 0; char *q = line + 5;
                    while (n < (int)sizeof raw_out) { long b = strtol(q, &e, 16); if (e == q) break; raw_out[n++] = (unsigned char)b; q = e; }
                    raw_n = n;
                    line = nl ? nl + 1 : NULL;
                    continue;
               }
               if (!strncmp(line, "dump ", 5)) {        /* debug: "dump <id> [words]" = rbp UI object */
                    int id = strtol(line + 5, &e, 0), nw = strtol(e, &e, 0);
                    if (nw <= 0 || nw > 48) nw = 24;
                    void *mgr = ((get_mgr_fn)0x001d0a30)();
                    uint32_t *ws = mgr ? ((get_active_fn)0x001cc510)(mgr) : NULL;
                    uint32_t *o = ws && ws[0] == WS_BROWSER_VFT ? ((get_obj_fn)0x0018dae8)(0, id) : NULL;
                    klog("flx4: dump %d = %p:", id, (void *)o);
                    for (int w = 0; o && w < nw; w++) klog(" %08x", o[w]);
                    klog("\n");
                    line = nl ? nl + 1 : NULL;
                    continue;
               }
               key = strtol(line, &e, 0);
               if (e != line) {
                    op = strtol(e, &e, 0);
                    ch = strtol(e, &e, 0);
                    param = strtol(e, &e, 0);
                    if (op == 9) {
                         send_key(key, OP_PRESS, ch, param);
                         usleep(80000);
                         send_key(key, OP_RELEASE, ch, param);
                    } else {
                         send_key_fl(key, op, ch, param, (float)param / 1023.0f, 0);   /* float like the knob path */
                    }
                    klog("flx4: inject key=0x%04lx op=%ld ch=%ld param=%ld\n", key, op, ch, param);
               }
               line = nl ? nl + 1 : NULL;
               usleep(50000);
          }
     }
     return NULL;
}

/* ---------------- USB1 self-heal ----------------
 * rbp silently drops the udev FIFO "mount" message until it has been up for
 * ~2-3 minutes, so a single notification at boot is lost. While a rekordbox
 * stick is mounted but rbp's USB1 detect flag is still 0, re-send
 * umount+mount every 10 s from inside rbp until it registers. */
#define DET_USB1 ((volatile uint32_t *)0x03256888)
static int stick_present(void)
{
     return usb1_file("PIONEER/rekordbox/export.pdb");
}
static void fifo_msg(const char *m)
{
     int fd = real_open("/tmp/udev_usb1", O_WRONLY | O_NONBLOCK);
     if (fd < 0) return;
     real_write(fd, m, strlen(m));
     real_close(fd);
}
/* uishim-rx3's EJECT USB1 button pauses the heal while it stops and unmounts the stick */
static volatile int heal_paused;
void knobshim_usb_heal_pause(int on) { heal_paused = on; }

/* Stuck: rbp has been up past its startup window (HEAL_STUCK_UPTIME) and still ignores HEAL_STUCK_TRIES
 * re-notifies. Seen once after a quick stick swap: only an rbp restart cleared it. uishim-rx3 then offers
 * "USB1 STUCK - HOLD TO RESTART" on the SOURCE screen (runs /root/wand/rbp-restart.sh). */
#define HEAL_STUCK_TRIES  6
#define HEAL_STUCK_UPTIME 240
static volatile int heal_tries;
static unsigned long long heal_t0;
int knobshim_usb1_stuck(void)
{
     if (access("/tmp/knobshim.usb1stuck", F_OK) == 0) return 1;   /* test switch: shows the button */
     return heal_tries >= HEAL_STUCK_TRIES && now_ms() - heal_t0 > HEAL_STUCK_UPTIME * 1000ULL;
}

static void *usb_heal_thread(void *arg)
{
     (void)arg;
     int tries = 0;
     heal_t0 = now_ms();
     for (;;) {
          heal_tries = tries;
          sleep(tries ? 10 : 30);
          if (heal_paused || !get_key_manager() || !stick_present()) { tries = 0; continue; }
          if (*DET_USB1 != 0) {
               if (tries) klog("flx4: USB1 registered after %d re-notifies\n", tries);
               tries = 0;
               continue;
          }
          char d[96], m[128];
          usb1_dir(d, sizeof d);
          snprintf(m, sizeof m, "umount %s", d); fifo_msg(m);
          usleep(500000);
          snprintf(m, sizeof m, "mount %s", d); fifo_msg(m);
          tries++;
          if (verbose || tries == HEAL_STUCK_TRIES) klog("flx4: USB1 re-notify #%d\n", tries);
     }
     return NULL;
}


/* ---------------- FLX4 output: host keep-alive + channel level meters ----------------
 * Keep-alive: rekordbox/Mixxx send this SysEx every 200 ms (Mixxx: "reverse engineered with
 * Wireshark ... the query seems to double as a keep alive message").
 * Meters: rbp's own RX3 meter chain. mixerengine::MixerEngine (singleton at 0x011493c0)
 * ::getInputChLevelMono(input 0 = CH1, 1 = CH2) returns the channel peak as dBFS + 21
 * (INT_MIN = silent); ui::Mixer::MonoLvMeter::calcLedValue turns it into the RX3's 11 LEDs
 * (< -24 -> 0, > 14 -> 11, else DB_TO_LEVEL_LED_TABLE). The FLX4 shows 5 segments
 * (B0/B1 02 vv: green1 0x26.., green2 0x41.., orange1 0x57.., orange2 0x65.., red 0x77..). */
#define MIXER_ENGINE     (*(void **)0x011493c0)
typedef long (*ch_level_fn)(void *engine, int input);
#define MixerEngine_getInputChLevelMono ((ch_level_fn)0x00057920)
/* the level is a peak hold: the RX3 front-panel code reads it and clears it every frame
 * (nothing clears it here, so it stuck at the loudest peak) */
typedef void (*ch_clear_fn)(void *engine, int input);
#define MixerEngine_clearInputChLevels  ((ch_clear_fn)0x00057950)
/* channel headphone CUE state (what rbp actually monitors): the FLX4's CUE LEDs (90/91 54) follow it */
typedef int (*ch_cue_fn)(void *engine, int input);
#define MixerEngine_getMixerChHeadphoneCue ((ch_cue_fn)0x000575e8)
static const unsigned char rx3_db_to_led[39] = {     /* rbp DB_TO_LEVEL_LED_TABLE, v = -24..14 */
     1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7, 7,
     8, 8, 8, 9, 9, 9, 10, 10, 10 };
/* RX3 LED count (0..11) -> FLX4 meter value: 1-3 green1, 4-6 green2, 7-8 orange1, 9-10 orange2,
 * 11 (RX3 clip LED) red */
static const unsigned char led_to_flx[12] = { 0x00, 0x2c, 0x36, 0x40, 0x48, 0x50, 0x56,
                                              0x5c, 0x64, 0x6c, 0x76, 0x7f };
static int rx3_leds(long v)
{
     if (v == (long)0x80000000) return 0;
     if (v > 14) return 11;
     if (v < -24) return 0;
     return rx3_db_to_led[v + 24];
}

static void *output_thread(void *arg)
{
     (void)arg;
     static const unsigned char ka[] = { 0xF0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x04, 0x05, 0x00, 0x50, 0x02, 0xF7 };
     int fd = -1, meters = !getenv("FLX_NO_METERS"), last[2] = { -1, -1 };
     for (unsigned tick = 0;; tick++) {
          if (fd < 0) {
               char dev[64];
               if (find_flx4_dev(dev, sizeof(dev)) == 0) fd = real_open(dev, O_WRONLY | O_NONBLOCK);
               if (fd < 0) { sleep(2); continue; }
               klog("flx4: keep-alive%s -> %s\n", meters ? " + meters" : "", dev);
               last[0] = last[1] = -1;
               mc_led_due = 1;
          }
          int err = 0;
          if (tick % 8 == 0 && real_write(fd, ka, sizeof ka) < 0 && errno != EAGAIN) err = 1;
          static int mc_off = -1;                       /* /tmp/knobshim.nomcled: stop forcing (and turn the LED off) */
          /* MONO SPLIT: audioshim builds the whole headphone feed (cue mono left, master mono right); the FLX4's
           * master path would add the stereo master to both ears on top, so the LED (= that path) stays off */
          if (tick % 20 == 0) { int o = getenv("FLX_NO_MC_LED") || access("/tmp/knobshim.nomcled", F_OK) == 0 ||
                                        (audioshim_hp_split && audioshim_hp_split());
                                if (o != mc_off) { mc_off = o; mc_led_due = 1; klog("flx4: MASTER CUE LED forcing %s\n", o ? "off" : "on"); } }
          if (mc_led_due || (!mc_off && tick % 40 == 0)) {   /* MASTER CUE LED on = master path live */
               unsigned char mcl[3] = { 0x96, 0x63, (unsigned char)(mc_off ? 0x00 : 0x7F) };
               if (!(mc_off && !mc_led_due) && real_write(fd, mcl, 3) < 0 && errno != EAGAIN) err = 1;
               mc_led_due = 0;
          }
          if (MIXER_ENGINE && get_key_manager()) {    /* CUE LEDs = rbp's channel headphone cue (send changes, 1 s refresh) */
               static int cue_last[2] = { -1, -1 };
               for (int c = 0; c < 2 && !err; c++) {
                    int on = MixerEngine_getMixerChHeadphoneCue(MIXER_ENGINE, c) != 0;
                    if (on == cue_last[c] && tick % 40) continue;
                    if (on != cue_last[c] && verbose) klog("flx4: ch%d cue %s (LED)\n", c + 1, on ? "on" : "off");
                    unsigned char m[3] = { (unsigned char)(0x90 + c), 0x54, (unsigned char)(on ? 0x7F : 0x00) };
                    if (real_write(fd, m, 3) < 0 && errno != EAGAIN) err = 1;
                    cue_last[c] = on;
               }
          }
          if (raw_n > 0) { int n = raw_n; if (real_write(fd, raw_out, n) < 0 && errno != EAGAIN) err = 1; raw_n = 0;
                           klog("flx4: raw MIDI out (%d bytes)\n", n); }
          void *eng = MIXER_ENGINE;
          if (meters && eng && get_key_manager()) {
               for (int c = 0; c < 2 && !err; c++) {
                    long raw = MixerEngine_getInputChLevelMono(eng, c);
                    MixerEngine_clearInputChLevels(eng, c);
                    static int shown[2];                      /* instant rise, ~1 LED / 50 ms fall */
                    int leds = rx3_leds(raw);
                    if (leds >= shown[c]) shown[c] = leds;
                    else if (tick % 2 == 0) shown[c]--;
                    int val = led_to_flx[shown[c]];
                    if (getenv("FLX_METER_LOG") && tick % 4 == 0) klog("flx4: meter ch%d raw=%ld leds=%d\n", c + 1, raw, leds);
                    if (val == last[c] && tick % 40) continue;     /* send changes; refresh every 1 s */
                    unsigned char m[3] = { (unsigned char)(0xB0 + c), 0x02, (unsigned char)val };
                    if (real_write(fd, m, 3) < 0 && errno != EAGAIN) err = 1;
                    last[c] = val;
               }
          }
          if (err) { real_close(fd); fd = -1; continue; }
          usleep(25000);
     }
     return NULL;
}

__attribute__((constructor)) static void flx4_init(void)
{
     if (!is_rbp_process()) return;
     pthread_t t;
     /* rbp reads /tmp/udev_usb1 (touchshim redirects Pioneer's /proc/udev_usb1 there; init.d's bridge feeds it)
      * and may miss early messages: keep re-notifying until det_usb1 registers. FLX_NO_USB_HEAL=1 disables. */
     if (!getenv("FLX_NO_USB_HEAL") && pthread_create(&t, NULL, usb_heal_thread, NULL) == 0) pthread_detach(t);
     if (pthread_create(&t, NULL, inject_thread, NULL) == 0) pthread_detach(t);
     if (pthread_create(&t, NULL, midi_thread, NULL) == 0) pthread_detach(t);
     if (!getenv("FLX_NO_KEEPALIVE") && pthread_create(&t, NULL, output_thread, NULL) == 0) pthread_detach(t);
     if (pthread_create(&t, NULL, jog_idle_thread, NULL) == 0) pthread_detach(t);
}
