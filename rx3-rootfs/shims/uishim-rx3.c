/*
 * uishim-rx3.c — on-screen RX3 top-row buttons, composited into rbp's own frame.
 *
 * rbp's display task (DS_HW_UpdateScreen) blits each dirty UI window onto the layer's primary
 * DirectFB surface (layer context 0x0245b810, DS surface at +128, IDirectFBSurface* at DS+28)
 * and then calls primary->Flip(NULL, WAIT|ONSYNC), which copies the back buffer to fb0.
 * This shim wraps that one Flip pointer: before each real Flip it GPU-blits a pre-rendered
 * button strip into the back buffer, so the buttons are part of every frame rbp presents
 * (no overlay plane, no flicker, rbp file untouched).
 * (An earlier attempt borrowed rbp's VIDEO UI slot; dropped.)
 *
 * IDirectFB* is captured by interposing DirectFBCreate (rbp imports it from libdirectfb).
 * DirectFB 1.4 interface slots used (byte offsets, verified against rbp's own calls):
 *   IDirectFB:        CreateSurface +32
 *   IDirectFBSurface: GetPixelFormat +32, Lock +52, Unlock +60, Flip +64, SetBlittingFlags +120, Blit +124
 * Labels use rbp's own UI text font, /root/gui/pset/fontdata/NS_FONT_ID_ISO8859_w.bin (2-bit
 * anti-aliased bitmaps: 27-row cells of 7-byte rows = 28 px, from character 0x20); a built-in 5x7
 * bitmap font is the fallback if the file can't be read.
 *
 * UISHIM_OFF=1 disables. Log: /root/wand/uishim.log. Inert outside rbp.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <errno.h>
#include <signal.h>
#include "rbp_process.h"
#include "label_font.h"   /* pre-rendered button labels (tools/gen_labels.py) */

/* rbp v1.20 (Pioneer's 4f2efcfc..., same addresses in the PrimeBox build 3706c68f...) */
#define DS_LAYER0_CTX     0x0245b810u
#define DS_CTX_PRIMARY    128               /* DS surface of the layer's screen */
#define DS_SURF_IFACE     28                /* IDirectFBSurface* inside a DS surface */
#define BROWSE_MODE       (*(volatile int *)0x0326f8b8)

/* IDirectFB / IDirectFBSurface slots */
#define DFB_CreateSurface     32
#define SURF_GetPixelFormat   32
#define SURF_Lock             52
#define SURF_Unlock           60
#define SURF_Flip             64
#define SURF_SetBlittingFlags 120
#define SURF_Blit             124
#define SURF_SetColor         84
#define SURF_SetDrawingFlags  144
#define SURF_FillRectangle    148
#define SLOT(iface, off)      (*(void **)((char *)(iface) + (off)))

typedef int (*create_surface_fn)(void *dfb, const void *desc, void **ret);
typedef int (*get_format_fn)(void *s, uint32_t *fmt);
typedef int (*lock_fn)(void *s, int flags, void **ptr, int *pitch);
typedef int (*unlock_fn)(void *s);
typedef int (*flip_fn)(void *s, const void *region, int flags);
typedef int (*set_bflags_fn)(void *s, int flags);
typedef int (*blit_fn)(void *s, void *src, const void *rect, int x, int y);
typedef int (*set_color_fn)(void *s, int r, int g, int b, int a);
typedef int (*set_dflags_fn)(void *s, int flags);
typedef int (*fill_rect_fn)(void *s, int x, int y, int w, int h);


/* Layouts (screen pixels):
 *  deck view (browseMode 1): wide bar over the REMAIN progress bar up to the time readout;
 *  other screens: bar between the screen title and the timer;
 *  search (browseMode 14, header full): toggle over the unused grey recording-time readout,
 *  tapping it drops down a vertical stack of the same buttons. */
#define NBTN      6
#define MODE_DECK   1
#define MODE_SEARCH 14
#define MODE_INFO   5                  /* INFO panel over the deck screen: same deck panels and top row */
#define MODE_TIMER  2                  /* countdown timer settings panel over the deck screen (tap the header timer) */
#define deck_screen(m) ((m) == MODE_DECK || (m) == MODE_INFO || (m) == MODE_TIMER)
#define DECK_X    236               /* BACK chevron at 188-228 */
#define DECK_Y    2
#define DECK_W    836
#define DECK_H    44
#define DECK_GAP  8
#define BAR_Y     5
#define BAR_H     40
#define BAR_GAP   8
#define TOG_X     1075
#define TOG_Y     4
#define TOG_W     95
#define TOG_H     36
#define STACK_X   1075
#define STACK_Y   46
#define STACK_W   195
#define STACK_BH  50
#define STACK_GAP 4
#define STACK_N   NBTN
#define STACK_H   (STACK_N * STACK_BH + (STACK_N - 1) * STACK_GAP)
/* POWER OFF (hold 2 s): right end of the UTILITY screen's header (shown while that window is up) */
#define PW_X      1070
#define PW_Y      4
#define PW_W      196
#define PW_H      38
#define HOLD_MS   2000
#define PW_CTRL   (NBTN + 1)           /* hit() id of the power control */
#define WIN_UTILITY_ID 2834            /* UTILITY sub-window (type 20, shown flag +12) */
/* TRACK FILTER (RX3 TRACK FILTER/EDIT key 0x420f) at the right end of the list's column header on BROWSE and
 * PLAYLIST (the track column is always the rightmost one). Tap = filter on/off, hold 1 s = filter edit screen.
 * rbp draws its own funnel + match count at the left of that header while the filter is on. */
#define FLT_X     1140
#define FLT_Y     55
#define FLT_W     130
#define FLT_H     40
#define FLT_HOLD_MS 1000
#define FLT_CTRL  (NBTN + 2)
#define K_TRACK_FILTER 0x420f
#define MODE_BROWSE   3
#define MODE_PLAYLIST 13
#define WIN_MENU_ID   540             /* MY SETTINGS popup: covers the header's right end */
#define IsEnableFilterOnOff ((int (*)(void))0x00113dc8)   /* reads globals only: safe from the Flip thread */
#define IsFilterOn          ((int (*)(void))0x00113ea4)
/* Deck-screen controls (browseMode 1), deck 1 coordinates; deck 2 is DK_DX to the right.
 * rbp draws QUANTIZE (only while on), the tempo range box and the MASTER tag in the BPM box itself; touching
 * them sends the RX3 deck key. SLIP / MASTER TEMPO have no indicator in rbp (on the RX3 they are lit buttons),
 * so they are drawn as buttons at the right end of the track-title row. States come from rbp's PlayEngine:
 * singleton *0x011497d0, players vector at +12/+16 (index 0 = deck 1), per player: quantize byte +0xedc,
 * master tempo byte +0xe9a, slip mode bit 0 of +0xf59, JOG MODE vinyl byte +0xead (plain reads, as
 * PlayEngine::isQuantizing / isVinylMode etc. do). VINYL is on by default, as on the RX3. */
#define DK_DX       640
#define DK_BTN_Y    591                 /* title row y 585-633, background (32,32,32) */
#define DK_BTN_W    62
#define DK_BTN_H    36
#define DK_BTN_TY   562
#define DK_BTN_TH   74
#define DK_VINYL_X  426
#define DK_SLIP_X   494
#define DK_MT_X     562
#define DK_QOFF_X   12                  /* QUANTIZE slot under SINGLE: label caps y 737-745, value y 755-778 */
#define DK_QOFF_Y   728
#define DK_QOFF_W   92
#define DK_QOFF_H   26
#define DK_CTRL     (NBTN + 3)          /* hit ids DK_CTRL + deck * DK_N + control */
enum { DK_SLIP, DK_MT, DK_VINYL, DK_QUANT, DK_RANGE, DK_MASTER, DK_N };
static const int dk_keys[DK_N] = { 0x4110, 0x4108, 0x4104, 0x410b, 0x4107, 0x4111 };
static const char *dk_names[DK_N] = { "SLIP", "MASTER TEMPO", "VINYL", "QUANTIZE", "TEMPO RANGE", "MASTER" };
static const int dk_rect[DK_N][4] = {   /* touch areas, deck 1 */
    /* buttons y 591-627; touch y 562-636 (after the touch calibration, taps on them landed y 565-602)
     * and across the 6 px gaps between them */
    { DK_SLIP_X - 3, DK_BTN_TY, DK_BTN_W + 6, DK_BTN_TH },
    { DK_MT_X - 3, DK_BTN_TY, DK_BTN_W + 6, DK_BTN_TH },
    { DK_VINYL_X - 3, DK_BTN_TY, DK_BTN_W + 6, DK_BTN_TH },
    { 12, 728, 92, 60 },                /* QUANTIZE indicator */
    { 452, 638, 64, 34 },               /* tempo range box x 461-507, y 645-664 */
    { 519, 642, 104, 66 },              /* BPM box x 521-619, y 645-704 (MASTER tag) */
};
#define PLAY_ENGINE (*(uint32_t **)0x011497d0)
static int player_flag(int deck, int off, int mask)
{
    uint32_t *e = PLAY_ENGINE;
    if (!e || !e[3] || e[4] <= e[3] || (int)((e[4] - e[3]) / 4) <= deck) return -1;
    uint8_t *pl = *(uint8_t **)(uintptr_t)(e[3] + deck * 4);
    return pl ? (pl[off] & mask) != 0 : -1;
}
/* BACK (RX3 BACK key 0x420d): a chevron button left of SOURCE. Deck screen: x 188-228 (right of the REMAIN
 * timer); other screens: first of the seven equal buttons (render_layouts); not on the search screen. */
#define BK_KEY    0x420d
#define BK_CTRL   200
#define BK_DECK_X 188
#define BK_DECK_W 40
#define MSG_W     600
#define MSG_H     140

static const char *labels[NBTN] = { "SOURCE", "BROWSE", "TAG LIST", "PLAYLIST", "SEARCH", "MENU" };
/* RX3 key codes (confirmed by injection). MENU 0x0206 / BACK 0x420d are not on screen. */
static const int keys[NBTN] = { 0x0201, 0x0202, 0x0203, 0x0204, 0x0205, 0x0206 };
#define BTN_MENU  5                    /* tap = MENU, hold 2 s = UTILITY (long press, like the RX3 button) */
/* deck screen / INFO row: the six buttons + SHORTCUT (RX3 key 0x0210, opens the SHORTCUT screen, browseMode 10) */
#define DECK_N    (NBTN + 1)
#define BTN_SHORTCUT NBTN
#define SC_CTRL   202                  /* hit() id of SHORTCUT */
#define SC_KEY    0x0210
/* SHORTCUT screen: MIXER MODE "MIDI" hands the mixer to DJ software on a PC (none here), which would take the FLX4's
 * mixer controls away from rbp's internal mixer. Its button (x 1120-1220, y 511-560) is dimmed and ignores touch.
 * (UTILITY's MIXER MODE is a multi-step list choice and is left alone.) */
#define MODE_SHORTCUT 10
#define MIDI_X    1120
#define MIDI_Y    511
#define MIDI_W    101
#define MIDI_H    50
#define BLOCK_CTRL 203                 /* hit() id: swallow the touch, do nothing */

static void ulog(const char *fmt, ...)
{
    static int fd = -1;
    if (fd < 0) fd = syscall(SYS_openat, AT_FDCWD, "/root/wand/uishim.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char b[256]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n > 0) syscall(SYS_write, fd, b, n < (int)sizeof b ? n : (int)sizeof b - 1);
}

/* ---------------- 5x7 font (A-Z, space) ---------------- */
static const uint8_t font5x7[27][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, {0,0,0,0,0,0,0},
};

/* ---------------- rendering (CPU, once) into 0xRRGGBB buffers ---------------- */
struct img { int w, h; uint32_t *px; };
static uint32_t deck_px[DECK_H * DECK_W], stack_px[STACK_H * STACK_W], tog_px[2][TOG_H * TOG_W];
static struct img img_deck = { DECK_W, DECK_H, deck_px };
static struct img img_stack = { STACK_W, STACK_H, stack_px };
static struct img img_tog[2] = { { TOG_W, TOG_H, tog_px[0] }, { TOG_W, TOG_H, tog_px[1] } };

static const uint32_t C_BG = 0x000000, C_FACE = 0x2e2e2e, C_EDGE = 0x5a5a5a, C_TEXT = 0xe6e6e6,
                      C_LIT = 0x4a4a4a, C_LITEDGE = 0x9a9a9a;

static void put(struct img *m, int x, int y, uint32_t c) { if (x >= 0 && x < m->w && y >= 0 && y < m->h) m->px[y * m->w + x] = c; }

static void text(struct img *m, int cx, int cy, const char *s, int sc)
{
    int n = strlen(s), tw = (n * 6 - 1) * sc, tx = cx - tw / 2, ty = cy - 7 * sc / 2;
    for (int i = 0; i < n; i++) {
        int g = s[i] == ' ' ? 26 : s[i] - 'A';
        if (g < 0 || g > 26) continue;
        for (int r = 0; r < 7 * sc; r++)
            for (int c = 0; c < 5 * sc; c++)
                if (font5x7[g][r / sc] & (0x10 >> (c / sc))) put(m, tx + i * 6 * sc + c, ty + r, C_TEXT);
    }
}

static void button(struct img *m, int x0, int y0, int w, int h, int lit)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            int e = y == y0 || y == y0 + h - 1 || x == x0 || x == x0 + w - 1;
            put(m, x, y, e ? (lit ? C_LITEDGE : C_EDGE) : (lit ? C_LIT : C_FACE));
        }
}

/* ---------------- rbp's UI font (NS_FONT_ID_ISO8859_w.bin) ---------------- */
#define NSF_PATH  "/root/gui/pset/fontdata/NS_FONT_ID_ISO8859_w.bin"
#define NSF_H     27                              /* rows per glyph cell */
#define NSF_W     28                              /* pixels per row (7 bytes, 2 bits each) */
#define NSF_CAP_SRC 19                            /* capital height in the file (rows 3..21) */
#define NSF_CAP     16                            /* target: capital height of DELAY in the Beat FX panel */
#define NSF_TRACK 2                               /* pixels between glyphs */
#define NSF_WEIGHT 1.35f                          /* coverage boost: keeps DELAY's 2 px stroke weight after scaling */
#define NSF_SPACE 6                               /* advance of ' ' */
static uint8_t *nsf;
static long nsf_len;

static void nsf_load(void)
{
    int fd = syscall(SYS_openat, AT_FDCWD, NSF_PATH, O_RDONLY, 0);
    if (fd < 0) { ulog("uishim: %s missing, bitmap labels\n", NSF_PATH); return; }
    static uint8_t buf[128 * 1024];
    long n = syscall(SYS_read, fd, buf, sizeof buf);
    syscall(SYS_close, fd);
    if (n < 0x60 * NSF_H * 7) { ulog("uishim: %s too short (%ld)\n", NSF_PATH, n); return; }
    nsf = buf; nsf_len = n;
    ulog("uishim: labels use rbp font %s (%ld bytes)\n", NSF_PATH, n);
}

static int nsf_px(int c, int x, int y)            /* 0..3 coverage */
{
    long o = ((long)(c - 0x20) * NSF_H + y) * 7 + x / 4;
    if (c < 0x20 || o >= nsf_len) return 0;
    return (nsf[o] >> (6 - 2 * (x % 4))) & 3;
}

static void nsf_extent(int c, int *x0, int *x1)  /* ink columns of a glyph */
{
    *x0 = NSF_W; *x1 = -1;
    for (int y = 0; y < NSF_H; y++)
        for (int x = 0; x < NSF_W; x++)
            if (nsf_px(c, x, y)) { if (x < *x0) *x0 = x; if (x > *x1) *x1 = x; }
}

/* coverage (0..1) of a source glyph over the rectangle [fx0,fx1) x [fy0,fy1) (area average) */
static float nsf_cov(int c, float fx0, float fx1, float fy0, float fy1)
{
    float sum = 0;
    for (int y = (int)fy0; y < fy1 && y < NSF_H; y++)
        for (int x = (int)fx0; x < fx1 && x < NSF_W; x++) {
            int a = nsf_px(c, x, y);
            if (!a) continue;
            float ox = (x + 1 < fx1 ? x + 1 : fx1) - (x > fx0 ? x : fx0);
            float oy = (y + 1 < fy1 ? y + 1 : fy1) - (y > fy0 ? y : fy0);
            sum += a / 3.0f * ox * oy;
        }
    return sum / ((fx1 - fx0) * (fy1 - fy0));
}

static uint32_t mixf(uint32_t bg, uint32_t fg, float a)
{
    uint32_t r = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        float b = (bg >> sh) & 0xff, f = (fg >> sh) & 0xff;
        r |= (uint32_t)(b + (f - b) * a + 0.5f) << sh;
    }
    return r;
}

/* label at (x, cy): x is the centre (align 0) or the left edge (align 1); capitals are `cap` px */
static void nsf_text_ex(struct img *m, int x, int cy, const char *s, int cap, uint32_t color, int align,
                        int track, int space)
{
    const float k = (float)cap / NSF_CAP_SRC;      /* target px per source px */
    int n = strlen(s), w = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == ' ') { w += space + track; continue; }
        int x0, x1; nsf_extent((uint8_t)s[i], &x0, &x1);
        if (x1 >= x0) w += (int)((x1 - x0 + 1) * k + 0.5f) + track;
    }
    w -= track;
    int px = align ? x : x - w / 2;
    float top = cy - cap / 2.0f - 3 * k;          /* source row 3 = top of capitals */
    int th = (int)(NSF_H * k + 1);
    for (int i = 0; i < n; i++) {
        if (s[i] == ' ') { px += space + track; continue; }
        int c = (uint8_t)s[i], x0, x1; nsf_extent(c, &x0, &x1);
        if (x1 < x0) continue;
        int tw = (int)((x1 - x0 + 1) * k + 0.5f);
        for (int ty = 0; ty < th; ty++)
            for (int tx = 0; tx < tw; tx++) {
                float a = nsf_cov(c, x0 + tx / k, x0 + (tx + 1) / k, ty / k, (ty + 1) / k) * NSF_WEIGHT;
                int X = px + tx, Y = (int)top + ty;
                if (a > 0.02f && X >= 0 && X < m->w && Y >= 0 && Y < m->h)
                    m->px[Y * m->w + X] = mixf(m->px[Y * m->w + X], color, a > 1 ? 1 : a);
            }
        px += tw + track;
    }
}

static void nsf_text(struct img *m, int cx, int cy, const char *s) { nsf_text_ex(m, cx, cy, s, NSF_CAP, C_TEXT, 0, NSF_TRACK, NSF_SPACE); }

static void label_text(struct img *m, int cx, int cy, const char *s)
{
    if (nsf) nsf_text(m, cx, cy, s);
    else text(m, cx, cy, s, 2);
}


/* Sound Color FX name in the deck boxes' loop row (the free space right of the loop length), deck view
 * only, centred, over a bar showing that deck's CFX knob: from the centre to the left (low side) or right
 * (high side), length = distance from 12 o'clock. Row background RGB565 0x18c3 = (24,24,24); loop rows at
 * screen y 218-259 (DECK 1) / 439-480 (DECK 2). */
#define CFX_X    96                  /* loop-length area reserved to x 113; longest values (1/32, 512) end ~x 92 */
#define CFX_Y1   222
#define CFX_Y2   443
#define CFX_W    80                  /* x 96-176, centre 136 */
#define CFX_H    34
#define CFX_CAP  12                  /* "DUB ECHO" ~72 px at 12 px caps, 1 px tracking */
#define CFX_BG_R 24
#define CFX_BAR_R 0x10              /* bar colour: the UI's blue */
#define CFX_BAR_G 0x5a
#define CFX_BAR_B 0xb4
static const char *cfx_names[6] = { "SPACE", "DUB ECHO", "SWEEP", "NOISE", "CRUSH", "FILTER" };
static uint32_t cfx_px[6][CFX_H * CFX_W];
static struct img img_cfx[6];
extern int knobshim_cfx(void) __attribute__((weak));
extern int knobshim_cfx_knob(int deck) __attribute__((weak));

/* labels as ARGB: coverage -> alpha, colour C_TEXT (rendered white-on-black, luminance = coverage) */
static void render_cfx(void)
{
    for (int t = 0; t < 6; t++) {
        img_cfx[t].w = CFX_W; img_cfx[t].h = CFX_H; img_cfx[t].px = cfx_px[t];
        for (int i = 0; i < CFX_W * CFX_H; i++) cfx_px[t][i] = 0;
        if (nsf) nsf_text_ex(&img_cfx[t], CFX_W / 2, CFX_H / 2, cfx_names[t], CFX_CAP, 0xffffff, 0, 1, 4);
        else text(&img_cfx[t], CFX_W / 2, CFX_H / 2, cfx_names[t], 1);
        for (int i = 0; i < CFX_W * CFX_H; i++) {
            uint32_t a = cfx_px[t][i] & 0xff;
            cfx_px[t][i] = a << 24 | (C_TEXT & 0xffffff);
        }
    }
}

/* MENU and SHORTCUT are icon buttons (gear, lightning bolt), ICON_W wide and full bar height (40-44 px): about
 * 38 x 32 CSS px on this ~215 ppi panel, well above WCAG 2.2 SC 2.5.8 (24 x 24); the text buttons share the rest. */
#define ICON_W 52
static int is_icon(int b) { return b == BTN_MENU || b == BTN_SHORTCUT; }
static void deck_bx(int b, int w, int gap, int *bx, int *bw)   /* deck row: button b's x / width in a row w wide */
{
    int tw = (w - 2 * ICON_W - (DECK_N - 1) * gap) / (DECK_N - 2), x = 0;
    for (int i = 0; i < b; i++) x += (is_icon(i) ? ICON_W : tw) + gap;
    *bx = x; *bw = is_icon(b) ? ICON_W : tw;
}
static int inpoly(float x, float y, const float *p, int n)
{
    int c = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if (((p[2 * i + 1] > y) != (p[2 * j + 1] > y)) &&
            x < (p[2 * j] - p[2 * i]) * (y - p[2 * i + 1]) / (p[2 * j + 1] - p[2 * i + 1]) + p[2 * i])
            c = !c;
    return c;
}
static void draw_icon(struct img *m, int x0, int w, int which)  /* 0 = gear (MENU), 1 = lightning bolt (SHORTCUT) */
{
    float cx = x0 + w / 2.0f, cy = m->h / 2.0f, R = m->h * 0.30f;
    static const float bolt[] = { 0.28f, -1.0f, -0.72f, 0.16f, -0.06f, 0.16f, -0.28f, 1.0f, 0.72f, -0.16f, 0.06f, -0.16f };
    for (int y = 0; y < m->h; y++)
        for (int x = x0; x < x0 + w; x++) {
            int n = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float fx = x + (sx + 0.5f) / 4 - cx, fy = y + (sy + 0.5f) / 4 - cy;
                    if (which == 0) {                       /* gear: 8 teeth, ring with a hole (no libm) */
                        float ax = fx < 0 ? -fx : fx, ay = fy < 0 ? -fy : fy;
                        if (ay > ax) { float t = ax; ax = ay; ay = t; }   /* fold into 0..45 deg */
                        float ratio = ax > 0 ? ay / ax : 0;               /* tan(angle) */
                        int tooth = ratio < 0.194f || ratio > 0.674f;     /* teeth at 0 / 45 deg, +-11 deg */
                        float outer = R * (tooth ? 1.0f : 0.78f), d2 = fx * fx + fy * fy;
                        if (d2 <= outer * outer && d2 >= R * R * 0.34f * 0.34f) n++;
                    } else if (inpoly(fx / R, fy / R, bolt, 6)) n++;
                }
            if (n) m->px[y * m->w + x] = mixf(m->px[y * m->w + x], C_TEXT, n / 16.0f);
        }
}
static void render_row(struct img *m, int gap)   /* deck screen row: DECK_N buttons */
{
    for (int i = 0; i < m->w * m->h; i++) m->px[i] = C_BG;
    for (int b = 0; b < DECK_N; b++) {
        int x0, bw;
        deck_bx(b, m->w, gap, &x0, &bw);
        button(m, x0, 0, bw, m->h, 0);
        if (is_icon(b)) draw_icon(m, x0, bw, b == BTN_SHORTCUT);
        else label_text(m, x0 + bw / 2, m->h / 2, labels[b]);
    }
}

/* power button: face + the power symbol (ring open at the top, vertical bar), 4x4 supersampled */
static uint32_t flt_px[2][FLT_W * FLT_H];
static uint32_t dk_px[6][DK_BTN_W * DK_BTN_H], qoff_px[DK_QOFF_W * DK_QOFF_H];
static struct img img_dk[6] = { { DK_BTN_W, DK_BTN_H, dk_px[0] }, { DK_BTN_W, DK_BTN_H, dk_px[1] },
                                { DK_BTN_W, DK_BTN_H, dk_px[2] }, { DK_BTN_W, DK_BTN_H, dk_px[3] },
                                { DK_BTN_W, DK_BTN_H, dk_px[4] }, { DK_BTN_W, DK_BTN_H, dk_px[5] } };
static struct img img_qoff = { DK_QOFF_W, DK_QOFF_H, qoff_px };
static struct img img_flt[2] = { { FLT_W, FLT_H, flt_px[0] }, { FLT_W, FLT_H, flt_px[1] } };
static uint32_t pw_px[1][PW_W * PW_H], msg_px[2][MSG_W * MSG_H];
static struct img img_pw[1] = { { PW_W, PW_H, pw_px[0] } };
static struct img img_msg[2] = { { MSG_W, MSG_H, msg_px[0] }, { MSG_W, MSG_H, msg_px[1] } };
static void render_messages(void)
{
    for (int t = 0; t < 2; t++) {
        for (int y = 0; y < MSG_H; y++)
            for (int x = 0; x < MSG_W; x++)
                msg_px[t][y * MSG_W + x] = (y < 2 || y >= MSG_H - 2 || x < 2 || x >= MSG_W - 2) ? 0xcdcacd
                                           : t ? 0x105ab4 : 0x202020;
        const struct label_bitmap *lb = &power_labels[t];
        int x0 = (MSG_W - lb->w) / 2, y0 = (MSG_H - MSG_CAP) / 2 - lb->cap_top;
        for (int y = 0; y < lb->h; y++)
            for (int x = 0; x < lb->w; x++) {
                int a = lb->a[y * lb->w + x], X = x0 + x, Y = y0 + y;
                if (a && X >= 0 && X < MSG_W && Y >= 0 && Y < MSG_H)
                    msg_px[t][Y * MSG_W + X] = mixf(msg_px[t][Y * MSG_W + X], C_TEXT, a / 255.0f);
            }
    }
}

/* FILTER button: funnel icon (4x4 supersampled) + label; lit = the UI's blue face (filter on) */
static void render_filter(void)
{
    const int ix = 14, iy = 11, iw = 22, ih = 19;  /* icon box */
    for (int t = 0; t < 2; t++) {
        struct img *m = &img_flt[t];
        uint32_t face = t ? 0x105ab4 : 0x393c39;
        for (int y = 0; y < FLT_H; y++)
            for (int x = 0; x < FLT_W; x++)
                m->px[y * FLT_W + x] = (y == 0 || y == FLT_H - 1 || x == 0 || x == FLT_W - 1) ? 0xcdcacd : face;
        for (int y = 0; y < ih; y++)
            for (int x = 0; x < iw; x++) {
                int n = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        float fx = x + (sx + 0.5f) / 4 - iw / 2.0f, fy = y + (sy + 0.5f) / 4;
                        float half = fy < 10 ? iw / 2.0f - fy * (iw / 2.0f - 3) / 10 : 3;   /* cone, then stem */
                        if (fx > -half && fx < half) n++;
                    }
                if (n) m->px[(iy + y) * FLT_W + ix + x] = mixf(m->px[(iy + y) * FLT_W + ix + x], C_TEXT, n / 16.0f);
            }
        if (nsf) nsf_text_ex(m, ix + iw + 10, FLT_H / 2, "FILTER", NSF_CAP, C_TEXT, 1, NSF_TRACK, NSF_SPACE);
        else text(m, ix + iw + 10 + 35, FLT_H / 2, "FILTER", 2);
    }
}

/* SLIP / MT / VINYL buttons (index = control * 2 + on): off = dark face, grey label; on = rbp's indicator red-orange
 * (248,68,24, as the tempo range box and QUANTIZE), white label. QUANTIZE placeholder: dim label (off). */
static void render_deck_controls(void)
{
    static const char *lbl[3] = { "SLIP", "MT", "VINYL" };
    for (int i = 0; i < 6; i++) {
        struct img *m = &img_dk[i];
        int on = i & 1;
        uint32_t face = on ? 0xf84418 : 0x393c39, edge = on ? 0xf84418 : 0x6a6a6a;
        for (int y = 0; y < DK_BTN_H; y++)
            for (int x = 0; x < DK_BTN_W; x++)
                m->px[y * DK_BTN_W + x] = (y == 0 || y == DK_BTN_H - 1 || x == 0 || x == DK_BTN_W - 1) ? edge : face;
        if (nsf) nsf_text_ex(m, DK_BTN_W / 2, DK_BTN_H / 2, lbl[i / 2], 14, on ? 0xf8fcf8 : 0x9a9a9a, 0, 2, 4);
        else text(m, DK_BTN_W / 2, DK_BTN_H / 2, lbl[i / 2], 2);
    }
    for (int i = 0; i < DK_QOFF_W * DK_QOFF_H; i++) qoff_px[i] = 0x000000;
    if (nsf) nsf_text_ex(&img_qoff, DK_QOFF_W / 2, 13, "QUANTIZE", 9, 0x484848, 0, 1, 3);
}

/* BACK buttons: button face + "<" chevron (3 px stroke, 4x4 supersampled) */
static uint32_t bk_px[1][BK_DECK_W * DECK_H];
static struct img img_bk[1] = { { BK_DECK_W, DECK_H, bk_px[0] } };
static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = px - ax - t * vx, dy = py - ay - t * vy;
    return dx * dx + dy * dy;
}
static void draw_chevron(struct img *m, int x0, int w)
{
    float cx = x0 + w / 2.0f, cy = m->h / 2.0f, hs = m->h * 0.24f, hw = hs * 0.6f, r = 1.5f;
    for (int y = 1; y < m->h - 1; y++)
        for (int x = x0 + 1; x < x0 + w - 1; x++) {
                int n = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4;
                        if (seg_dist(fx, fy, cx + hw, cy - hs, cx - hw, cy) <= r * r ||
                            seg_dist(fx, fy, cx - hw, cy, cx + hw, cy + hs) <= r * r) n++;
                    }
                if (n) m->px[y * m->w + x] = mixf(m->px[y * m->w + x], C_TEXT, n / 16.0f);
        }
}

static void render_back(void)
{
    for (int i = 0; i < 1; i++) {
        struct img *m = &img_bk[i];
        for (int k = 0; k < m->w * m->h; k++) m->px[k] = C_BG;
        button(m, 0, 0, m->w, m->h, 0);
        draw_chevron(m, 0, m->w);
    }
}

/* Top bar on every screen but the deck screen and search: BACK + the six buttons, seven equal buttons, the same
 * bar on every screen. It sits between the longest fixed header text on the left ("Please select a source" ends
 * x 320; the Track Filter title + count end x 325 after move_filter_header) and the countdown timer (x 970).
 * On BROWSE / PLAYLIST it covers part of the artist / playlist name. */
#define NROW    (NBTN + 1)             /* index 0 = BACK */
#define ROW_X0  334
#define ROW_X1  962
#define ROW_GAP 6
/* BACK is 45 x 40: on this 7" 1280x800 panel (~215 ppi, ~1.35 px per CSS px / dp) that is ~33 x 30 CSS px,
 * above WCAG 2.2 SC 2.5.8 Target Size (Minimum, AA) of 24 x 24 CSS px (~33 px) without the spacing exception */
#define ROW_BK_W 45
/* MENU (row index BTN_MENU + 1) is the gear icon (ICON_W); the five text buttons share the rest (99 px) */
#define ROW_BW  ((ROW_X1 - ROW_X0 - ROW_BK_W - ICON_W - NBTN * ROW_GAP) / (NBTN - 1))
static int row_w(int b) { return b == 0 ? ROW_BK_W : b == BTN_MENU + 1 ? ICON_W : ROW_BW; }
static int row_bx(int b) { int x = 0; for (int i = 0; i < b; i++) x += row_w(i) + ROW_GAP; return x; }
static uint32_t row_px[BAR_H * (ROW_X1 - ROW_X0)];
static struct img img_row = { ROW_X1 - ROW_X0, BAR_H, row_px };
static int nsf_width(const char *s, int cap, int track, int space)
{
    const float k = (float)cap / NSF_CAP_SRC;
    int w = 0;
    for (; *s; s++) {
        if (*s == ' ') { w += space + track; continue; }
        int x0, x1; nsf_extent((uint8_t)*s, &x0, &x1);
        if (x1 >= x0) w += (int)((x1 - x0 + 1) * k + 0.5f) + track;
    }
    return w - track;
}

static void render_layouts(void)
{
    struct img *m = &img_row;
    for (int i = 0; i < m->w * m->h; i++) m->px[i] = C_BG;
    for (int b = 0; b < NROW; b++) {
        int x0 = row_bx(b);
        button(m, x0, 0, row_w(b), m->h, 0);
        if (b == 0) { draw_chevron(m, x0, ROW_BK_W); continue; }
        if (b == BTN_MENU + 1) { draw_icon(m, x0, ICON_W, 0); continue; }
        const char *t = labels[b - 1];
        if (!nsf) { text(m, x0 + ROW_BW / 2, m->h / 2, t, 2); continue; }
        int track = NSF_TRACK;                   /* tighten the spacing when a label is too wide for the button */
        while (track > 0 && nsf_width(t, NSF_CAP, track, NSF_SPACE) > ROW_BW - 6) track--;
        nsf_text_ex(m, x0 + ROW_BW / 2, m->h / 2, t, NSF_CAP, C_TEXT, 0, track, NSF_SPACE);
    }
}

static void render_all(void)
{
    render_row(&img_deck, DECK_GAP);
    for (int i = 0; i < STACK_W * STACK_H; i++) stack_px[i] = C_BG;
    for (int b = 0; b < STACK_N; b++) {
        int y0 = b * (STACK_BH + STACK_GAP);
        button(&img_stack, 0, y0, STACK_W, STACK_BH, 0);
        label_text(&img_stack, STACK_W / 2, y0 + STACK_BH / 2, labels[b]);
    }
    for (int i = 0; i < PW_W * PW_H; i++) pw_px[0][i] = C_BG;       /* POWER OFF (UTILITY header) */
    button(&img_pw[0], 0, 0, PW_W, PW_H, 0);
    label_text(&img_pw[0], PW_W / 2, PW_H / 2, "POWER OFF");
    render_messages();
    render_filter();
    render_deck_controls();
    render_back();
    render_layouts();
    for (int o = 0; o < 2; o++) {                  /* toggle: closed / open, hamburger icon */
        for (int i = 0; i < TOG_W * TOG_H; i++) tog_px[o][i] = C_BG;
        button(&img_tog[o], 0, 0, TOG_W, TOG_H, o);
        for (int l = 0; l < 3; l++)
            for (int y = 0; y < 3; y++)
                for (int x = 0; x < 34; x++) put(&img_tog[o], TOG_W / 2 - 17 + x, 9 + l * 8 + y, C_TEXT);
    }
}

/* ---------------- DirectFB hook ---------------- */
static void *g_dfb;            /* IDirectFB* (from DirectFBCreate) */
static void *g_primary;        /* rbp's layer-0 primary IDirectFBSurface* */
static void *g_deck, *g_stack, *g_tog[2], *g_cfx[6], *g_pw[1], *g_msg[2], *g_flt[2], *g_dk[6], *g_qoff, *g_bk[1], *g_row;   /* our surfaces (primary pixel format) */
static flip_fn real_flip;
static volatile int strip_on = 1;
static volatile int stack_open;              /* drop-down open (non-deck screens) */

int DirectFBCreate(void **ret)
{
    static int (*real)(void **);
    if (!real) real = dlsym(RTLD_NEXT, "DirectFBCreate");
    int r = real(ret);
    if (r == 0 && ret && rbp_process_check()) { g_dfb = *ret; ulog("uishim: IDirectFB %p\n", g_dfb); }
    return r;
}

static void *make_surface(void *primary, struct img *m)
{
    uint32_t fmt = 0;
    ((get_format_fn)SLOT(primary, SURF_GetPixelFormat))(primary, &fmt);
    int bpp = (fmt >> 20) & 0x0f;                 /* DFB_BYTES_PER_PIXEL */
    struct { int flags, caps, width, height; uint32_t pixelformat; int pad[12]; } desc;
    memset(&desc, 0, sizeof desc);
    desc.flags = 0x2 | 0x4 | 0x8;                 /* DSDESC_WIDTH | HEIGHT | PIXELFORMAT */
    desc.width = m->w; desc.height = m->h; desc.pixelformat = fmt;
    void *s = NULL;
    int r = ((create_surface_fn)SLOT(g_dfb, DFB_CreateSurface))(g_dfb, &desc, &s);
    ulog("uishim: surface %dx%d format %08x -> %d %p\n", m->w, m->h, fmt, r, s);
    if (r || !s) return NULL;
    void *p; int pitch;
    if (((lock_fn)SLOT(s, SURF_Lock))(s, 2 /* DSLF_WRITE */, &p, &pitch) == 0) {
        for (int y = 0; y < m->h; y++) {
            uint8_t *row = (uint8_t *)p + y * pitch;
            for (int x = 0; x < m->w; x++) {
                uint32_t c = m->px[y * m->w + x];
                if (bpp == 2)
                    ((uint16_t *)row)[x] = ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
                else
                    ((uint32_t *)row)[x] = 0xff000000u | c;
            }
        }
        ((unlock_fn)SLOT(s, SURF_Unlock))(s);
    }
    return s;
}

static void *make_surface_argb(struct img *m)       /* DSPF_ARGB, alpha from the buffer */
{
    struct { int flags, caps, width, height; uint32_t pixelformat; int pad[12]; } desc;
    memset(&desc, 0, sizeof desc);
    desc.flags = 0x2 | 0x4 | 0x8;
    desc.width = m->w; desc.height = m->h; desc.pixelformat = 0x00418c04;   /* DSPF_ARGB */
    void *s = NULL;
    if (((create_surface_fn)SLOT(g_dfb, DFB_CreateSurface))(g_dfb, &desc, &s) || !s) return NULL;
    void *p; int pitch;
    if (((lock_fn)SLOT(s, SURF_Lock))(s, 2, &p, &pitch) == 0) {
        for (int y = 0; y < m->h; y++) memcpy((uint8_t *)p + y * pitch, &m->px[y * m->w], m->w * 4);
        ((unlock_fn)SLOT(s, SURF_Unlock))(s);
    }
    return s;
}

static void fill_alpha(void *dst, int r, int g, int b, int a, int x, int y, int w, int h)   /* blended */
{
    ((set_dflags_fn)SLOT(dst, SURF_SetDrawingFlags))(dst, 1 /* DSDRAW_BLEND */);
    ((set_color_fn)SLOT(dst, SURF_SetColor))(dst, r, g, b, a);
    ((fill_rect_fn)SLOT(dst, SURF_FillRectangle))(dst, x, y, w, h);
    ((set_dflags_fn)SLOT(dst, SURF_SetDrawingFlags))(dst, 0);
}

static void fill(void *dst, int r, int g, int b, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    ((set_dflags_fn)SLOT(dst, SURF_SetDrawingFlags))(dst, 0);
    ((set_color_fn)SLOT(dst, SURF_SetColor))(dst, r, g, b, 0xff);
    ((fill_rect_fn)SLOT(dst, SURF_FillRectangle))(dst, x, y, w, h);
}

/* one deck box's CFX cell: background, knob bar from the centre, name blended on top */
/* Stacked: top = CFX name over the knob bar (low / high side), bottom = the CFX PARAMETER bar (orange fill from the
 * left, 0..1; the RX3's single PARAMETER knob, so both decks show the same value). Drag / tap in the cell sets it. */
#define CFX_KH   22                  /* knob-bar part height */
#define CFX_PY   28                  /* parameter bar: y offset / height inside the cell; flush with the deck box bottom (last row y 259 / 480) */
#define CFX_PH   10
#define CFXP_X0  12                  /* PARAMETER bar spans the deck box: x 12-176, under the loop indicator (ends y ~245) */
#define CFXP_W   (CFX_X + CFX_W - CFXP_X0)
extern float knobshim_cfx_param(void) __attribute__((weak));
/* what PARAMETER does per effect (manual p. 98-99), shown inside the bar; index = cfx_names order */
static const char *cfxp_names[6] = { "FEEDBACK", "FEEDBACK", "LEVEL", "NOISE", "LEVEL", "RESONANCE" };
static uint32_t cfxp_px[6][CFX_PH * (CFX_X + CFX_W - 12)];
static struct img img_cfxp[6];
static void *g_cfxp[6];
static void render_cfxp(void)                      /* ARGB labels: white, alpha = coverage */
{
    int w = CFX_X + CFX_W - 12;
    for (int t = 0; t < 6; t++) {
        img_cfxp[t].w = w; img_cfxp[t].h = CFX_PH; img_cfxp[t].px = cfxp_px[t];
        for (int i = 0; i < w * CFX_PH; i++) cfxp_px[t][i] = 0;
        if (nsf) nsf_text_ex(&img_cfxp[t], w / 2, CFX_PH / 2, cfxp_names[t], 8, 0xffffff, 0, 1, 3);
        for (int i = 0; i < w * CFX_PH; i++) {
            uint32_t a = cfxp_px[t][i] & 0xff;
            cfxp_px[t][i] = a << 24 | 0xf8fcf8;
        }
    }
}
static void draw_cfx_cell(void *dst, int y, int deck, int type)
{
    fill(dst, CFX_BG_R, CFX_BG_R, CFX_BG_R, CFX_X, y, CFX_W, CFX_H);
    int v = knobshim_cfx_knob ? knobshim_cfx_knob(deck) : -1, d = v < 0 ? 0 : v - 512;
    if (d > 16 || d < -16) {
        int half = CFX_W / 2, len = (d < 0 ? -d : d) * half / 511;
        if (len > half) len = half;
        fill(dst, CFX_BAR_R, CFX_BAR_G, CFX_BAR_B, d > 0 ? CFX_X + half : CFX_X + half - len, y + 1, len, CFX_KH - 2);
    }
    if (g_cfx[type]) {                             /* name centred on the knob-bar part */
        ((set_bflags_fn)SLOT(dst, SURF_SetBlittingFlags))(dst, 1 /* DSBLIT_BLEND_ALPHACHANNEL */);
        ((blit_fn)SLOT(dst, SURF_Blit))(dst, g_cfx[type], NULL, CFX_X, y + CFX_KH / 2 - CFX_H / 2);
        ((set_bflags_fn)SLOT(dst, SURF_SetBlittingFlags))(dst, 0);
    }
    float p = knobshim_cfx_param ? knobshim_cfx_param() : 0.5f;
    int pw = (int)(p * CFXP_W + 0.5f);
    fill(dst, 56, 56, 56, CFXP_X0, y + CFX_PY, CFXP_W, CFX_PH);          /* track */
    if (pw > 0) fill(dst, 0xf8, 0x44, 0x18, CFXP_X0, y + CFX_PY, pw, CFX_PH);   /* PARAMETER (rbp's indicator orange) */
    if (type >= 0 && type < 6 && g_cfxp[type]) {   /* what the parameter does for this effect */
        ((set_bflags_fn)SLOT(dst, SURF_SetBlittingFlags))(dst, 1 /* DSBLIT_BLEND_ALPHACHANNEL */);
        ((blit_fn)SLOT(dst, SURF_Blit))(dst, g_cfxp[type], NULL, CFXP_X0, y + CFX_PY);
        ((set_bflags_fn)SLOT(dst, SURF_SetBlittingFlags))(dst, 0);
    }
}

static void blit(void *dst, void *src, int x, int y)
{
    if (!src) return;
    ((set_bflags_fn)SLOT(dst, SURF_SetBlittingFlags))(dst, 0);
    ((blit_fn)SLOT(dst, SURF_Blit))(dst, src, NULL, x, y);
}

typedef void (*refresh_ws_fn)(void);
#define ui_com_draw_RefreshWinscape ((refresh_ws_fn)0x0018e214)   /* queue a full redraw of the screen */

/* ---------------- EJECT USB1 / USB2 (source screen) ----------------
 * Like holding the RX3's USB STOP: key 0x8002 on the device's channel (1 = USB1, 2 = USB2; one
 * UsbStorageManager per slot: press mutes the players and notifies the players + DbProxy of the disconnect), then
 * sync + unmount so the stick can be pulled. Shown under MY SETTINGS MENU while the SOURCE window (object 2686) is
 * shown; it follows the highlighted SOURCE row (USB1 row y 100-190, USB2 y 192-282; highlight = (0,125,230)). */
#define EJ_X 756
#define EJ_Y 610
#define EJ_W 504
#define EJ_H 60
/* touch area: the button (y 610-670) plus margin, up to MY SETTINGS MENU (ends y 592) and the deck strip (y 710).
 * (Was shifted down to y 640-745 for the touch offset touchshim now corrects.) */
#define EJ_TY 600
#define EJ_TH 90
/* mount points: Pioneer's udev rule mounts a stick at /media/usbN/<kernel name> (sda1, sdb1, ... whichever the
 * kernel assigned this boot), so look them up in /proc/mounts; the last ones seen are kept for the eject logic */
static char usb_dir[3][64] = { "", "/media/usb1/sda1", "/media/usb2/sda1" }, usb_disk[3][16] = { "", "sda", "sdb" };
static volatile int usb_mnt[3];                    /* slot currently mounted */
static void usb_refresh(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    char line[256], dev[64], mnt[64];
    int seen[3] = { 0, 0, 0 };
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%63s %63s", dev, mnt) != 2 || strncmp(mnt, "/media/usb", 10) != 0) continue;
        int n = mnt[10] - '0';
        if ((n != 1 && n != 2) || mnt[11] != '/') continue;
        seen[n] = 1;
        snprintf(usb_dir[n], sizeof usb_dir[n], "%s", mnt);
        if (strncmp(dev, "/dev/", 5) == 0) {               /* /dev/sdb1 -> sdb */
            snprintf(usb_disk[n], sizeof usb_disk[n], "%.15s", dev + 5);
            for (char *q = usb_disk[n]; *q; q++) if (*q >= '0' && *q <= '9') { *q = 0; break; }
        }
    }
    fclose(f);
    usb_mnt[1] = seen[1]; usb_mnt[2] = seen[2];
}
static int usb1_has_pioneer(void)
{
    char p[96];
    snprintf(p, sizeof p, "%s/PIONEER", usb_dir[1]);
    return access(p, F_OK) == 0;
}
#define WIN_SOURCE_ID 2686
#define DET_USB1   (*(volatile int *)0x03256888)
enum { EJ_IDLE, EJ_STOPPING, EJ_SAFE, EJ_BUSY, EJ_N };
static uint32_t ej_px[2][EJ_N][EJ_H * EJ_W];   /* [slot - 1][state] */
static struct img img_ej[2][EJ_N];
static void *g_ej[2][EJ_N];
static volatile int ej_state = EJ_IDLE, ej_visible, src_visible, usb_mounted, ej_slot = 1, ej_busy_slot = 1;
static volatile int cal_on;                        /* touch calibration targets shown (/tmp/uishim.cal) */
static volatile int util_visible;                  /* UTILITY window shown (for POWER OFF) */
static volatile int flt_visible;                   /* TRACK FILTER header button shown */
extern void knobshim_usb_heal_pause(int on) __attribute__((weak));
typedef void *(*get_mgr_fn)(void);
typedef void *(*get_active_fn)(void *);
typedef void *(*get_obj_fn)(void *, int);
#define WS_BROWSER_VFT 0x004c8290u

/* styled like rbp's MY SETTINGS MENU button above it (measured): 1 px border (205,202,205),
 * face (57,60,57), 17 px capitals; SAFE TO REMOVE uses the UI's blue face */
static void render_eject(void)
{
    for (int sl = 0; sl < 2; sl++)
    for (int t = 0; t < EJ_N; t++) {
        uint32_t face = t == EJ_SAFE ? 0x105ab4 : 0x393c39, *px = ej_px[sl][t];
        img_ej[sl][t].w = EJ_W; img_ej[sl][t].h = EJ_H; img_ej[sl][t].px = px;
        for (int y = 0; y < EJ_H; y++)
            for (int x = 0; x < EJ_W; x++)
                px[y * EJ_W + x] = (y == 0 || y == EJ_H - 1 || x == 0 || x == EJ_W - 1) ? 0xcdcacd : face;
        /* label: same face / size / spacing as rbp's pre-drawn MY SETTINGS MENU label, centred like it */
        const struct label_bitmap *lb = &eject_labels[sl * EJ_N + t];
        int x0 = (EJ_W - lb->w) / 2, y0 = (EJ_H - LABEL_CAP) / 2 - lb->cap_top;
        for (int y = 0; y < lb->h; y++)
            for (int x = 0; x < lb->w; x++) {
                int a = lb->a[y * lb->w + x], X = x0 + x, Y = y0 + y;
                if (a && X >= 0 && X < EJ_W && Y >= 0 && Y < EJ_H)
                    px[Y * EJ_W + X] = mixf(px[Y * EJ_W + X], C_TEXT, a / 255.0f);
            }
    }
}

/* USB1 STUCK: knobshim's USB1 heal reports that rbp ignores the stick (knobshim_usb1_stuck: rbp up > 4 min and
 * 6 re-notifies in a row ignored; seen once after a quick stick swap, only an rbp restart cleared it). Shown in
 * place of EJECT on the SOURCE screen; holding it STUCK_HOLD_MS runs /root/wand/rbp-restart.sh (restarts rbp
 * as apl_start does, re-announces the sticks, selects USB1). Red face so it is not mistaken for EJECT. */
extern int knobshim_usb1_stuck(void) __attribute__((weak));
#define STUCK_CTRL    225
#define STUCK_HOLD_MS 2000
static uint32_t stuck_px[3][EJ_H * EJ_W];          /* 0 = STUCK, 1 = RESTARTING, 2 = KEEP HOLDING */
static struct img img_stuck[3] = { { EJ_W, EJ_H, stuck_px[0] }, { EJ_W, EJ_H, stuck_px[1] }, { EJ_W, EJ_H, stuck_px[2] } };
static void *g_stuck[3];
static void ts_refresh(int d);
static volatile int stuck_visible, restarting;
static void render_stuck(void)
{
    for (int t = 0; t < 3; t++) {
        uint32_t face = t == 1 ? 0x393c39 : 0x9c1c1c;
        for (int y = 0; y < EJ_H; y++)
            for (int x = 0; x < EJ_W; x++)
                stuck_px[t][y * EJ_W + x] = (y == 0 || y == EJ_H - 1 || x == 0 || x == EJ_W - 1) ? 0xcdcacd : face;
        const struct label_bitmap *lb = &stuck_labels[t];
        int x0 = (EJ_W - lb->w) / 2, y0 = (EJ_H - LABEL_CAP) / 2 - lb->cap_top;
        for (int y = 0; y < lb->h; y++)
            for (int x = 0; x < lb->w; x++) {
                int a = lb->a[y * lb->w + x], X = x0 + x, Y = y0 + y;
                if (a && X >= 0 && X < EJ_W && Y >= 0 && Y < EJ_H)
                    stuck_px[t][Y * EJ_W + X] = mixf(stuck_px[t][Y * EJ_W + X], C_TEXT, a / 255.0f);
            }
    }
}
/* the script kills rbp: run it in its own session with none of rbp's descriptors (an inherited ALSA or FIFO fd
 * would keep the devices busy for the new rbp). Raw syscalls only in the child (no shim hooks after fork). */
static void spawn_restart(void)
{
    static char *const argv[] = { "sh", "/root/wand/rbp-restart.sh", NULL };
    static char *const envp[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", NULL };
    pid_t p = fork();
    if (p == 0) {
        syscall(SYS_setsid);
        for (int fd = 0; fd < 1024; fd++) syscall(SYS_close, fd);
        syscall(SYS_execve, "/bin/sh", argv, envp);
        syscall(SYS_exit, 127);
    }
    ulog("uishim: USB1 stuck -> rbp-restart.sh (pid %d)\n", (int)p);
}

/* ---------------- TAG TRACK/REMOVE (INFO screen) ----------------
 * RX3 key 0x420e. rbp's UiKey_AddTag acts on its "active deck" (0x0114ca08, 1/2: the last deck loaded), not on the
 * deck the INFO panel shows (0x0326f910, 0/1, set by the panel's DECK 1 / DECK 2). The button makes the INFO deck
 * active for the key and then puts the previous active deck back. A tap adds the track (INFO then shows a check
 * mark). On this firmware the key does not remove from INFO (tap or hold; manual p. 47 says a tap should) --
 * removing is a long press on the TAG LIST screen. The button turns blue while INFO's title shows the tag tick:
 * the tick's top-right (x 219-224, y 80-85) has ink, the note icon never reaches past x 217. */
#define TAG_KEY    0x420e
#define TAG_CTRL   201
#define TAG_W      320
#define TAG_H      60
#define TAG_X      836                /* centred under the artwork (x 876-1116, y 175-413) in the right panel */
#define TAG_Y      494                /* below the comment line (y ~435-460); panel ends y 568 */
#define ACTIVE_DECK (*(volatile int *)0x0114ca08)
#define INFO_DECK   (*(volatile int *)0x0326f910)
#define TAG_TICK_X 219
#define TAG_TICK_Y 80
static uint32_t tag_px[2][TAG_W * TAG_H];
static struct img img_tag[2] = { { TAG_W, TAG_H, tag_px[0] }, { TAG_W, TAG_H, tag_px[1] } };
static void *g_tag[2];
static void render_tag(void)
{
    for (int t = 0; t < 2; t++) {                  /* 0 = not tagged, 1 = tagged (the UI's blue, like FILTER on) */
        uint32_t face = t ? 0x105ab4 : 0x393c39;
        for (int y = 0; y < TAG_H; y++)
            for (int x = 0; x < TAG_W; x++)
                tag_px[t][y * TAG_W + x] = (y == 0 || y == TAG_H - 1 || x == 0 || x == TAG_W - 1) ? 0xcdcacd : face;
        const struct label_bitmap *lb = &tag_labels[0];
        int x0 = (TAG_W - lb->w) / 2, y0 = (TAG_H - LABEL_CAP) / 2 - lb->cap_top;
        for (int y = 0; y < lb->h; y++)
            for (int x = 0; x < lb->w; x++) {
                int a = lb->a[y * lb->w + x], X = x0 + x, Y = y0 + y;
                if (a && X >= 0 && X < TAG_W && Y >= 0 && Y < TAG_H)
                    tag_px[t][Y * TAG_W + X] = mixf(tag_px[t][Y * TAG_W + X], C_TEXT, a / 255.0f);
            }
    }
}
static int info_tagged(void *prim)                 /* INFO title row shows the tag tick (from the back buffer) */
{
    void *p; int pitch, n = 0;
    if (((lock_fn)SLOT(prim, SURF_Lock))(prim, 1 /* DSLF_READ */, &p, &pitch)) return 0;
    for (int y = TAG_TICK_Y; y < TAG_TICK_Y + 6; y++) {
        const uint16_t *row = (const uint16_t *)((uint8_t *)p + y * pitch);
        for (int x = TAG_TICK_X; x < TAG_TICK_X + 6; x++) if ((row[x] >> 11) > 17) n++;   /* bright ink */
    }
    ((unlock_fn)SLOT(prim, SURF_Unlock))(prim);
    return n >= 3;
}
static void *tag_worker(void *arg)
{
    (void)arg;
    void (*send)(int, int, int, long) = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
    int deck = INFO_DECK + 1, prev = ACTIVE_DECK;
    if (!send || deck < 1 || deck > 2) return NULL;
    ACTIVE_DECK = deck;
    send(TAG_KEY, 0, 1, 0);
    usleep(80000);
    send(TAG_KEY, 2, 1, 0);
    usleep(400000);                                /* rbp's UI task handles the key by now */
    if (ACTIVE_DECK == deck && prev != deck && (prev == 1 || prev == 2)) ACTIVE_DECK = prev;
    ulog("uishim: TAG TRACK deck %d (active deck %d)\n", deck, prev);
    return NULL;
}

static int window_shown(int id)
{
    void *mgr = ((get_mgr_fn)0x001d0a30)();
    uint32_t *ws = mgr ? ((get_active_fn)0x001cc510)(mgr) : NULL;
    if (!ws || ws[0] != WS_BROWSER_VFT) return 0;
    uint32_t *o = ((get_obj_fn)0x0018dae8)(0, id);
    return o && o[3];
}


static void *eject_worker(void *arg)
{
    (void)arg;
    int sl = ej_busy_slot;
    void (*send)(int, int, int, long) = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
    if (sl == 1 && knobshim_usb_heal_pause) knobshim_usb_heal_pause(1);
    if (send) {
        send(0x8002, 0, sl, 0);                   /* USB STOP press on that slot's channel */
        usleep(1000000);
        send(0x8002, 1, sl, 0);                   /* long press */
        usleep(50000);
        send(0x8002, 2, sl, 0);                   /* release */
    }
    if (sl == 1) for (int i = 0; i < 50 && DET_USB1 != 0; i++) usleep(100000);
    syscall(SYS_sync);
    /* rbp's USB STOP unmounts the stick itself (as on the RX3); only unmount if it is still mounted */
    for (int i = 0; i < 30 && (usb_refresh(), usb_mnt[sl]); i++) usleep(100000);
    int r = 0;
    if (usb_mnt[sl])
        for (int i = 0; i < 20 && (r = syscall(SYS_umount2, usb_dir[sl], 0)) != 0 && errno == EBUSY; i++) usleep(300000);
    usb_refresh();
    ulog("uishim: eject USB%d: mounted=%d umount -> %d (%d)\n", sl, usb_mnt[sl], r, r ? errno : 0);
    ej_state = r == 0 ? EJ_SAFE : EJ_BUSY;
    if (r != 0 && sl == 1 && knobshim_usb_heal_pause) knobshim_usb_heal_pause(0);
    ui_com_draw_RefreshWinscape();
    return NULL;
}

/* 4 Hz: source-screen visibility, stick state; redraw when the button changes */
static void *eject_state_thread(void *arg)
{
    (void)arg;
    int last = -1;
    while (BROWSE_MODE == 0) usleep(200000);
    for (;;) {
        usleep(250000);
        /* src_visible / util_visible are computed per frame in my_flip */
        {                                              /* /tmp/uishim.hide: draw nothing (to see rbp's own screen) */
            int c = access("/tmp/uishim.cal", F_OK) == 0;
            if (c != cal_on) { cal_on = c; ui_com_draw_RefreshWinscape(); ulog("uishim: calibration targets %s\n", c ? "on" : "off"); }
            int h = access("/tmp/uishim.hide", F_OK) == 0;
            if (h == strip_on) {                       /* debug only: on the deck screen this full redraw also blanks rbp's
                                                         * header (countdown / INFO) until the next screen change */
                strip_on = !h; ui_com_draw_RefreshWinscape(); ulog("uishim: overlay %s\n", h ? "hidden" : "shown");
                if (BROWSE_MODE == MODE_DECK) {        /* the full redraw blanks rbp's deck-screen header: BROWSE and
                                                         * back makes rbp repaint it (brief flash) */
                    void (*send)(int, int, int, long) = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
                    if (send) {
                        usleep(300000);
                        send(0x0202, 0, 1, 0); usleep(80000); send(0x0202, 2, 1, 0);
                        usleep(350000);
                        send(0x0202, 0, 1, 0); usleep(80000); send(0x0202, 2, 1, 0);
                    }
                }
            }
        }
        usb_refresh();
        if (ej_state == EJ_IDLE)                       /* the button follows the highlighted SOURCE row; with one
                                                         * stick in, SOURCE lists only it, in the first row */
            ej_busy_slot = usb_mnt[1] != usb_mnt[2] ? (usb_mnt[1] ? 1 : 2) : ej_slot;
        usb_mounted = ej_busy_slot == 1 ? usb1_has_pioneer() : usb_mnt[2];
        char blk[48];
        snprintf(blk, sizeof blk, "/sys/block/%s", usb_disk[ej_busy_slot]);
        if (ej_state == EJ_SAFE && access(blk, F_OK) != 0) {   /* pulled out */
            ej_state = EJ_IDLE;
            if (ej_busy_slot == 1 && knobshim_usb_heal_pause) knobshim_usb_heal_pause(0);
        }
        if (ej_state == EJ_BUSY && !usb_mounted) ej_state = EJ_IDLE;
        int now = src_visible ? ej_state : -1;
        if (now != last) last = now;           /* drawn on rbp's next frame; no forced redraw (screen switches race it) */
    }
    return NULL;
}

/* ---------------- TRACK SEARCH / SEARCH arrows, TAP, Beat FX QUANTIZE, browse TAG TRACK ----------------
 * Deck screen: a < and > beside each deck's TRACK number (number ink x 43-75 / 683-715, y 668-689). A tap is
 * TRACK SEARCH (0x4215 rev / 0x4214 fwd: the RX3 jumps to the start of the track, twice = previous); holding past
 * ARW_HOLD_MS is SEARCH (0x4120 / 0x411f) pressed until the finger lifts, like holding the RX3's SEARCH button:
 * press, then the long-press event (op 1) ARW_LONG_MS later -- rbp only scans a *paused* deck on the long press
 * (the press alone steps one frame); a playing deck scans from the press.
 * Beat FX panel (x 1090-1270): tap the BPM box = TAP (0x4492), hold 1 s = SHIFT + TAP (AUTO BPM, manual p. 102);
 * tap the QUANTIZE line = Beat FX QUANTIZE (0x0493). Only while the panel is drawn (frame pixels checked per frame).
 * Browse / playlist track lists: TAG TRACK (0x420e) left of FILTER tags the highlighted track. */
#define ARW_W       14
#define ARW_H       24
#define ARW_Y       667
#define ARW_LX      12                 /* glyphs, clear of a 3-digit number (x 36-84); touch areas wider (WCAG 2.2) */
#define ARW_RX      91
#define ARW_TY      646                /* TRACK label to SINGLE (y 646-722) */
#define ARW_TH      76
#define ARW_CTRL    210                /* hit ids ARW_CTRL + deck * 2 + dir (0 = back, 1 = forward) */
#define ARW_HOLD_MS 350
#define ARW_LONG_MS 400
static const int arw_tx[2][2] = { { 4, 50 }, { 64, 54 } };   /* touch x, w (deck 1): left / right half of the
                                                                TRACK box (x 10-105) + 12 px past it (a tap on >
                                                                landed at x 115), dead zone x 54-64 */
static const int arw_track[2] = { 0x4215, 0x4214 }, arw_search[2] = { 0x4120, 0x411f };
#define FXP_X       1100
#define TAP_Y       218
#define TAP_H       74                 /* BPM + msec lines only (y 218-292); BEAT line = gap */
#define FXQ_Y       318                /* QUANTIZE button y 328-350, touch 318-360 */
#define FXQ_H       42
#define FXP_W       160
#define TAP_CTRL    220                /* (210-213 are the arrows) */
#define FXQ_CTRL    221
#define K_TAP       0x4492
#define K_FXQUANT   0x0493
#define K_SHIFT     0x4103
#define BTAG_X      1004               /* FILTER is at 1140 */
#define BTAG_CTRL   222
#define FXQB_X 1106                    /* QUANTIZE button inside the BPM box (box x 1100-1260, bottom ~352) */
#define FXQB_Y 328
#define FXQB_W 148
#define FXQB_H 22
static uint32_t arw_px[4][ARW_W * ARW_H], btag_px[FLT_W * FLT_H], fxq_px[2][FXQB_W * FXQB_H];
static struct img img_arw[4] = { { ARW_W, ARW_H, arw_px[0] }, { ARW_W, ARW_H, arw_px[1] },
                                 { ARW_W, ARW_H, arw_px[2] }, { ARW_W, ARW_H, arw_px[3] } };
static struct img img_btag = { FLT_W, FLT_H, btag_px };
/* MASTER CUE indicator: the FLX4's MASTER CUE LED is kept lit (it gates the FLX4's hardware master path), so the RX3
 * MASTER CUE state is shown here: an orange bar with MASTER CUE in the 20 px gap under the Beat FX panel (y 363-383) */
#define MCI_X 1092
#define MCI_Y 360                       /* a tab on the Beat FX panel's bottom margin (panel ends y 362): 10 px above ZOOM (y 383), visible after the panel's 1280x800 -> 1024x600 scaling */
#define MCI_W 176
#define MCI_H 13
static uint32_t mci_px[MCI_W * MCI_H];
static struct img img_mci = { MCI_W, MCI_H, mci_px };
static void *g_mci;
extern int knobshim_master_cue(void) __attribute__((weak));
static struct img img_fxq[2] = { { FXQB_W, FXQB_H, fxq_px[0] }, { FXQB_W, FXQB_H, fxq_px[1] } };
static void *g_arw[4], *g_btag, *g_fxq[2];
static volatile int fx_visible;
static void render_search_controls(void)
{
    for (int i = 0; i < 4; i++) {                  /* index = dir * 2 + active; black like the deck info area */
        int dir = i / 2;
        uint32_t ink = (i & 1) ? 0xf8fcf8 : 0x9a9a9a;
        float tipx = dir ? ARW_W - 3.0f : 3.0f, backx = dir ? 3.5f : ARW_W - 3.5f;
        for (int y = 0; y < ARW_H; y++)
            for (int x = 0; x < ARW_W; x++) {
                int n = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4;
                        if (seg_dist(fx, fy, backx, 3, tipx, ARW_H / 2.0f) < 1.4f * 1.4f ||
                            seg_dist(fx, fy, tipx, ARW_H / 2.0f, backx, ARW_H - 3) < 1.4f * 1.4f) n++;
                    }
                arw_px[i][y * ARW_W + x] = mixf(0x000000, ink, n / 16.0f);
            }
    }
    for (int y = 0; y < FLT_H; y++)
        for (int x = 0; x < FLT_W; x++)
            btag_px[y * FLT_W + x] = (y == 0 || y == FLT_H - 1 || x == 0 || x == FLT_W - 1) ? 0xcdcacd : 0x393c39;
    if (nsf) nsf_text_ex(&img_btag, FLT_W / 2, FLT_H / 2, "TAG TRACK", NSF_CAP, C_TEXT, 0, NSF_TRACK, NSF_SPACE);
    for (int i = 0; i < MCI_W * MCI_H; i++) mci_px[i] = 0xf84418;   /* rbp's indicator red-orange */
    {
        const struct label_bitmap *lb = &mcue_labels[0];
        int x0 = (MCI_W - lb->w) / 2, y0 = (MCI_H - 10) / 2 - lb->cap_top;
        for (int y = 0; y < lb->h; y++)
            for (int x = 0; x < lb->w; x++) {
                int a = lb->a[y * lb->w + x], X = x0 + x, Y = y0 + y;
                if (a && X >= 0 && X < MCI_W && Y >= 0 && Y < MCI_H) mci_px[Y * MCI_W + X] = mixf(mci_px[Y * MCI_W + X], 0xf8fcf8, a / 255.0f);
            }
    }
    /* Beat FX QUANTIZE button, styled like rbp's STATUS / BEAT FX switch: on = grey (123,125,123) face with black
     * text, off = black face with grey text; both in its (98,101,98) border. Drawn over rbp's own QUANTIZE label. */
    for (int t = 0; t < 2; t++) {
        uint32_t face = t ? 0x7b7d7b : 0x000000, ink = t ? 0x000000 : 0x949594;   /* off text = CH SELECT's grey */
        for (int y = 0; y < FXQB_H; y++)
            for (int x = 0; x < FXQB_W; x++)
                fxq_px[t][y * FXQB_W + x] = (y == 0 || y == FXQB_H - 1 || x == 0 || x == FXQB_W - 1) ? 0x626562 : face;
        const struct label_bitmap *lb = &fxq_labels[0];   /* sized like the panel's CH SELECT */
        int x0 = (FXQB_W - lb->w) / 2, y0 = (FXQB_H - FXQ_CAP) / 2 - lb->cap_top;
        for (int y = 0; y < lb->h; y++)
            for (int x = 0; x < lb->w; x++) {
                int a = lb->a[y * lb->w + x], X = x0 + x, Y = y0 + y;
                if (a && X > 0 && X < FXQB_W - 1 && Y > 0 && Y < FXQB_H - 1)
                    fxq_px[t][Y * FXQB_W + X] = mixf(fxq_px[t][Y * FXQB_W + X], ink, a / 255.0f);
            }
    }
}
static int px_is(const uint16_t *row, int x, int r, int g, int b)   /* RGB565 pixel ~ (r,g,b) */
{
    int R = (row[x] >> 11) << 3, G = ((row[x] >> 5) & 63) << 2, B = (row[x] & 31) << 3;
    return abs(R - r) < 12 && abs(G - g) < 12 && abs(B - b) < 12;
}
static int fx_panel_shown(void *prim)              /* Beat FX panel frame: header (49,48,49), sides (32,32,32) */
{
    void *p; int pitch, ok;
    if (((lock_fn)SLOT(prim, SURF_Lock))(prim, 1 /* DSLF_READ */, &p, &pitch)) return 0;
    const uint16_t *r60 = (const uint16_t *)((uint8_t *)p + 60 * pitch), *r200 = (const uint16_t *)((uint8_t *)p + 200 * pitch),
                   *r250 = (const uint16_t *)((uint8_t *)p + 250 * pitch);
    ok = px_is(r60, 1095, 49, 48, 49) && px_is(r200, 1095, 32, 32, 32) && px_is(r250, 1265, 32, 32, 32) &&
         px_is(r250, 1104, 0, 0, 0);
    ((unlock_fn)SLOT(prim, SURF_Unlock))(prim);
    return ok;
}
/* Beat FX QUANTIZE state: UiGetMixBeatFxQuantizeOn (StatWatcher read, safe from any thread). The key 0x0493 is
 * not a toggle: like the SHORTCUT screen's ON / OFF switch it carries the new state as its param (1 on, 0 off). */
#define UiGetMixBeatFxQuantizeOn ((int (*)(void))0x000fe4e0)
static pthread_mutex_t arw_mx = PTHREAD_MUTEX_INITIALIZER;
static volatile int arw_id = -1, arw_fired;
static void *arw_thread(void *arg)
{
    int id = (int)(intptr_t)arg;
    usleep(ARW_HOLD_MS * 1000);
    void (*send)(int, int, int, long) = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
    pthread_mutex_lock(&arw_mx);
    if (arw_id == id && !arw_fired && send) {
        int d = (id - ARW_CTRL) / 2, dir = (id - ARW_CTRL) % 2;
        arw_fired = 1;
        send(arw_search[dir], 0, d + 1, 0);        /* SEARCH held until the finger lifts */
        ulog("uishim: deck %d SEARCH %s held\n", d + 1, dir ? "fwd" : "rev");
    }
    pthread_mutex_unlock(&arw_mx);
    usleep(ARW_LONG_MS * 1000);
    pthread_mutex_lock(&arw_mx);
    if (arw_id == id && arw_fired && send)         /* still held: long press (paused deck: continuous scan) */
        send(arw_search[(id - ARW_CTRL) % 2], 1, (id - ARW_CTRL) / 2 + 1, 0);
    pthread_mutex_unlock(&arw_mx);
    return NULL;
}
static void arw_down(int id)
{
    pthread_mutex_lock(&arw_mx);
    arw_id = id; arw_fired = 0;
    pthread_mutex_unlock(&arw_mx);
    pthread_t t;
    if (pthread_create(&t, NULL, arw_thread, (void *)(intptr_t)id) == 0) pthread_detach(t);
}
static void arw_up(void (*send)(int, int, int, long))
{
    pthread_mutex_lock(&arw_mx);
    int id = arw_id, fired = arw_fired;
    arw_id = -1;
    if (id >= 0 && send) {
        int d = (id - ARW_CTRL) / 2, dir = (id - ARW_CTRL) % 2;
        if (fired) send(arw_search[dir], 2, d + 1, 0);
        else { send(arw_track[dir], 0, d + 1, 0); usleep(80000); send(arw_track[dir], 2, d + 1, 0); }
        if (!fired) ulog("uishim: deck %d TRACK SEARCH %s\n", d + 1, dir ? "fwd" : "rev");
    }
    pthread_mutex_unlock(&arw_mx);
}

/* ---------------- Master Rec (header timer) ----------------
 * The header's Master Rec timer (dot + 00:00:00, x ~1076-1168) is the touch control (user's choice): not recording:
 * tap = MASTER REC (0x0401, starts recording to the stick in USB2); recording: tap = TRACK MARK (0x0402, splits the
 * file), hold 1 s = MASTER REC (stop) -- a stray tap never ends a recording. Recording state = the WavWriter's
 * "recording" byte (MixerEngine singleton +84 -> WavWriter +156, what MixerEngine::getRecordingStatus reads).
 * Active on every screen where rbp draws the timer; on the search screen the menu toggle sits there instead.
 * Recordings are listed by rbp under USB2 -> REC ("PIONEER DJ REC"), not in FOLDER. */
#define REC_CTRL   223
#define CFXP_CTRL  224                /* CFX PARAMETER slider in the deck boxes' CFX cell */
#define REC_X      1072
#define REC_Y      2
#define REC_W      104
#define REC_H      44
#define K_MASTER_REC 0x0401
#define K_TRACK_MARK 0x0402
static int rec_active(void)
{
    uint8_t *e = *(uint8_t **)0x011493c0;
    uint8_t *w = e ? *(uint8_t **)(e + 84) : 0;
    return w ? w[156] != 0 : 0;
}
static int rec_visible(int mode)
{
    /* every screen with rbp's header timer: all but SOURCE (12), SHORTCUT (10), UTILITY (7) and the search
     * screen (14, where the menu toggle sits on it) */
    return mode != 0 && mode != 7 && mode != 10 && mode != 12 && mode != MODE_SEARCH;
}

/* ---------------- POWER (hold 2 s) ----------------
 * Like holding the RX3's power switch: key 0x8001 (ui::KeyInput::isPowerOff = op 1) makes rbp prepare to power
 * off (it releases/unmounts the USB storage); the RX3 panel CPU would then cut power. Here: sync, show
 * SAFE TO UNPLUG and halt the system through init (/sbin/poweroff); the Wandboard cannot switch itself off. */
static volatile int pw_state;                      /* 0 idle, 1 powering off, 2 safe to unplug */
/* BROWSE / PLAYLIST: the category column on the left (x 0-100) has the blue highlight = BACK has nothing left to
 * go up to (computed per frame in my_flip) */
static volatile int side_focus;
/* PLAYLIST never moves the highlight to the left column: there BACK is sent, and when the screen title (x 108-330,
 * y 8-44; "PLAYLIST" at the top level, else the folder / playlist name) has not changed 500 ms later, BACK had
 * nothing to go up to -> PLAYLIST key = deck view. title_sig: pixel sum, updated per frame on that screen. */
static volatile unsigned title_sig, title_frames;
static void *back_playlist_worker(void *arg)
{
    (void)arg;
    void (*send)(int, int, int, long) = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
    if (!send) return NULL;
    unsigned sig = title_sig;
    send(0x420d, 0, 1, 0); usleep(80000); send(0x420d, 2, 1, 0);
    usleep(500000);
    if (BROWSE_MODE == MODE_PLAYLIST && title_sig == sig) {
        send(0x0204, 0, 1, 0); usleep(80000); send(0x0204, 2, 1, 0);
        ulog("uishim: BACK at PLAYLIST top -> deck\n");
    }
    return NULL;
}
static volatile int hold_ctrl = -1, hold_fired;    /* control being held (BTN_MENU / PW_CTRL / FLT_CTRL), hold time reached */
static volatile int hold_ms = HOLD_MS;
static volatile unsigned long long hold_t0;
static unsigned long long ms_now(void)
{
    struct { long s, ns; } ts;
    syscall(263 /* clock_gettime32 */, 1 /* CLOCK_MONOTONIC */, &ts);
    return (unsigned long long)ts.s * 1000 + ts.ns / 1000000;
}

static void *poweroff_worker(void *arg)
{
    (void)arg;
    void (*send)(int, int, int, long) = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
    pw_state = 1;
    ui_com_draw_RefreshWinscape();
    ulog("uishim: POWER OFF\n");
    if (knobshim_usb_heal_pause) knobshim_usb_heal_pause(1);
    if (send) {
        send(0x8001, 0, 1, 0);                    /* power key: press, long press, release */
        usleep(1000000);
        send(0x8001, 1, 1, 0);
        usleep(50000);
        send(0x8001, 2, 1, 0);
    }
    for (int i = 0; i < 60 && usb1_has_pioneer(); i++) usleep(100000);
    usleep(1000000);                               /* let rbp finish writing */
    syscall(SYS_sync);
    pw_state = 2;
    ui_com_draw_RefreshWinscape();
    usleep(800000);                                /* let the message reach the screen */
    signal(SIGTERM, SIG_IGN);                      /* init's TERM must not start rbp's own restart path */
    ulog("uishim: halting\n");
    if (fork() == 0) { execl("/sbin/poweroff", "poweroff", (char *)NULL); _exit(127); }
    return NULL;
}

/* hold: redraw the progress; after HOLD_MS: MENU sends its long press (UTILITY), POWER OFF powers off */
static void *hold_thread(void *arg)
{
    (void)arg;
    while (hold_ctrl >= 0 && !hold_fired) {
        if (ms_now() - hold_t0 >= (unsigned)hold_ms) {
            hold_fired = 1;
            if (hold_ctrl == FLT_CTRL) {             /* TRACK FILTER long press: edit screen */
                void (*send)(int, int, int, long) = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
                if (send) { send(K_TRACK_FILTER, 0, 1, 0); usleep(50000); send(K_TRACK_FILTER, 1, 1, 0);
                            usleep(50000); send(K_TRACK_FILTER, 2, 1, 0); }
                ulog("uishim: TRACK FILTER held -> edit\n");
            } else if (hold_ctrl == REC_CTRL) {          /* recording, held 1 s: MASTER REC = stop */
                void (*send)(int, int, int, long) = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
                if (send) { send(K_MASTER_REC, 0, 1, 0); usleep(80000); send(K_MASTER_REC, 2, 1, 0); }
                ulog("uishim: MASTER REC held -> stop\n");
            } else if (hold_ctrl == TAP_CTRL) {          /* TAP held: SHIFT + TAP = AUTO BPM */
                void (*send)(int, int, int, long) = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
                if (send) { send(K_SHIFT, 0, 1, 0); usleep(30000); send(K_TAP, 0, 1, 0); usleep(80000);
                            send(K_TAP, 2, 1, 0); usleep(30000); send(K_SHIFT, 2, 1, 0); }
                ulog("uishim: TAP held -> AUTO\n");
            } else if (hold_ctrl == STUCK_CTRL) {
                restarting = 1;
                ui_com_draw_RefreshWinscape();
                usleep(400000);                        /* let RESTARTING reach the screen */
                spawn_restart();
            } else if (hold_ctrl == PW_CTRL) {
                pthread_t t;
                if (pthread_create(&t, NULL, poweroff_worker, NULL) == 0) pthread_detach(t);
            } else {
                void (*send)(int, int, int, long) = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
                if (send) send(keys[BTN_MENU], 1 /* long press */, 1, 0);
                ulog("uishim: MENU held -> UTILITY\n");
            }
            break;
        }
        if (hold_ctrl == STUCK_CTRL) ts_refresh(0);   /* static SOURCE screen: make rbp present frames (progress) */
        usleep(50000);
    }
    /* no forced redraws here: RefreshWinscape from this thread while rbp switches screens left the
     * UTILITY screen half drawn, and at 10 Hz it made the header text flicker. The progress line is drawn
     * on whatever frames rbp presents. */
    return NULL;
}

static void draw_progress(void *dst, int x, int y, int w, int h)
{
    if (hold_ctrl < 0 || hold_fired) return;
    int p = (int)((ms_now() - hold_t0) * (w - 4) / hold_ms);
    fill(dst, 0x10, 0x5a, 0xb4, x + 2, y + h - 6, p > w - 4 ? w - 4 : p, 4);
}
static void row_rect(int b, int x0, int w, int gap, int *bx, int *bw)
{
    deck_bx(b, w, gap, bx, bw);
    *bx += x0;
}

static volatile int last_mode;

/* ---------------- deck title scroll (deck screen) ----------------
 * rbp draws each deck's track title with a text object (deck 1 = object 0x997, deck 2 = 0x9f0, vtable 0x4c804c;
 * rect words at +24/+28 = x1|y1<<16, x2|y2<<16 relative to the deck info window at x 10 / 650, y 581; text
 * pointer +52) and simply cuts long titles off. Here long titles ping-pong: the thread moves the object's x1
 * left/right (rbp redraws it in its own font via ui_com_draw_RefreshObject) and pauses at each end. x2 is set so
 * the text stops before the VINYL button. rbp draws the text from x1 under its note icon, so the flip hook covers
 * x 106-144 of the title row with a patch captured while the title is at rest. The end of the text is found from
 * the rendered pixels (no ink in the last TS_ENDGAP px before the right edge), so it matches rbp's font exactly.
 * The redraw is requested for the title's group (text id + 2, type 14: background 0x998/0x9f1, text, note icon
 * 0x996/0x9ef), so the background is repainted under the moving text. /tmp/uishim.noscroll stops it. */
#define TS_VFT     0x004c804cu
#define TS_X1      135               /* original x1 / x2 (relative) */
#define TS_X2      599
#define TS_X2_NEW  412               /* abs 422 / 1062: just left of VINYL (426) */
#define TS_WIN_Y   581
#define TS_ROW_Y   585               /* title row y 585-634 */
#define TS_ROW_H   49
#define TS_PATCH_X 96                /* relative: note icon strip x 106-144 */
#define TS_PATCH_W 39
#define TS_TXT_Y   596               /* ink rows of the title text */
#define TS_TXT_H   34
#define TS_ENDGAP  40
#define TS_ENDPAD  4                /* scroll stops with the text end this far before x2 (abs 418 / 1058; VINYL at 426) */
#define TS_STEP    2                 /* px per tick */
#define TS_TICK_MS 40
#define TS_PAUSE_MS 2000
typedef void (*refresh_obj_fn)(void *obj, int id);
#define ui_com_draw_RefreshObject ((refresh_obj_fn)0x0018e1a0)
static const int ts_id[2] = { 0x997, 0x9f0 }, ts_win_x[2] = { 10, 650 };
static const int ts_deckbox[2] = { 0x9af, 0xa08 };   /* "DECK n" box group, x 0-97: redrawn over text that
                                                        scrolled left past the title row */
enum { TS_IDLE, TS_LEFT, TS_PAUSE_END, TS_RIGHT, TS_BACK };
static struct {
    volatile int offset, state, is_long, at_end, patch_ok, need_measure, x2_set, est, width, ink_off, ink_r;
    const uint16_t *volatile str;           /* the title object's text (m_string, +52) */
    volatile unsigned long long t;
    uint32_t hash;
    void *patch;
} ts[2];
static volatile int ts_enabled = 1;

static uint32_t *ts_object(int d)
{
    void *mgr = ((get_mgr_fn)0x001d0a30)();
    uint32_t *ws = mgr ? ((get_active_fn)0x001cc510)(mgr) : NULL;
    if (!ws || ws[0] != WS_BROWSER_VFT) return NULL;
    uint32_t *o = ((get_obj_fn)0x0018dae8)(0, ts_id[d]);
    if (!o || o[0] != TS_VFT || (o[1] >> 16) != (uint32_t)ts_id[d]) return NULL;
    return o;
}

static void ts_set_x1(uint32_t *o, int x1)
{
    o[6] = (o[6] & 0xffff0000u) | ((uint32_t)x1 & 0xffff);
}

static void ts_refresh(int d)                  /* redraw the title group: background, text, note icon */
{
    uint32_t *g = ((get_obj_fn)0x0018dae8)(0, ts_id[d] + 2);
    ui_com_draw_RefreshObject(0, g && (g[1] & 0xffff) == 14 ? ts_id[d] + 2 : ts_id[d]);
    if (ts[d].offset > TS_X1 - TS_PATCH_X - 4) {  /* text reaches the DECK box: repaint that on top */
        uint32_t *b = ((get_obj_fn)0x0018dae8)(0, ts_deckbox[d]);
        if (b && (b[1] & 0xffff) == 14) ui_com_draw_RefreshObject(0, ts_deckbox[d]);
    }
}

/* rough width of a title in rbp's list font (ISO8859 glyph ink widths + 2 px; wider guess for other scripts).
 * rbp drops whole characters at the clip edge, so the pixel test alone can't tell "just fits" from "cut". */
static int ts_estimate(const uint16_t *w)
{
    int px = 0;
    for (int i = 0; w && i < 256 && w[i]; i++) {
        int c = w[i], x0, x1;
        if (c == ' ') { px += 8; continue; }
        if (c < 0x100 && nsf) { nsf_extent(c, &x0, &x1); px += x1 >= x0 ? x1 - x0 + 3 : 8; }
        else px += 22;
    }
    return px;
}

static void *title_thread(void *arg)
{
    (void)arg;
    while (BROWSE_MODE == 0) usleep(200000);
    unsigned long long mode_t = ms_now();
    int last = -1;
    for (;;) {
        usleep(TS_TICK_MS * 1000);
        int mode = BROWSE_MODE;
        if (mode != last) { last = mode; mode_t = ms_now(); }
        int ok = ts_enabled && strip_on && deck_screen(mode) && ms_now() - mode_t > 800   /* not while screens switch */
                 && access("/tmp/uishim.noscroll", F_OK) != 0;   /* overlay hidden: title back at rbp's position */
        for (int d = 0; d < 2; d++) {
            uint32_t *o = ts_object(d);
            if (!ok || !o) {                           /* off the deck screen / stopped: title back at rest */
                if (o && ts[d].offset) { ts_set_x1(o, TS_X1); if (deck_screen(mode)) ts_refresh(d); }
                ts[d].offset = 0; ts[d].state = TS_IDLE; ts[d].t = ms_now();
                ts[d].hash = 0;                        /* re-measure when back on the deck screen */
                ts[d].x2_set = o && (o[7] & 0xffff) == TS_X2_NEW;
                continue;
            }
            int x1 = (int16_t)(o[6] & 0xffff), x2 = o[7] & 0xffff;
            if (x1 != TS_X1 - ts[d].offset || (x2 != TS_X2 && x2 != TS_X2_NEW)) {   /* not what we expect */
                if (ts[d].state != TS_IDLE || ts[d].offset) ulog("uishim: title %d rect %d..%d unexpected, stop\n", d + 1, x1, x2);
                ts[d].offset = 0; ts[d].state = TS_IDLE;
                if (x1 != TS_X1) continue;
            }
            if (!ts[d].x2_set) {                       /* clip before SLIP */
                o[7] = (o[7] & 0xffff0000u) | TS_X2_NEW;
                ts[d].x2_set = 1;
                ts_refresh(d);
            }
            uint32_t h = 2166136261u;                  /* title changed? */
            const uint16_t *w = (const uint16_t *)(uintptr_t)o[13];
            for (int i = 0; w && i < 256 && w[i]; i++) h = (h ^ w[i]) * 16777619u;
            if (h != ts[d].hash) {
                ts[d].hash = h;
                if (ts[d].offset) { ts[d].offset = 0; ts_set_x1(o, TS_X1); ts_refresh(d); }
                ts[d].state = TS_IDLE; ts[d].t = ms_now(); ts[d].is_long = 0; ts[d].patch_ok = 0;
                ts[d].need_measure = w && w[0];        /* no track: nothing to measure */
                ts[d].est = ts_estimate(w);
                ts[d].width = 0;                       /* real width: measured at rest (ts_flip) */
                ts[d].str = w;
                continue;
            }
            unsigned long long now = ms_now();
            int prev = ts[d].offset;
            switch (ts[d].state) {
            case TS_IDLE:
                if (ts[d].need_measure && now - ts[d].t > 400 && (now / TS_TICK_MS) % 12 == 0)
                    ts_refresh(d);   /* present a frame so the flip hook can measure */
                if (ts[d].is_long && ts[d].patch_ok && !ts[d].need_measure && now - ts[d].t > TS_PAUSE_MS) {
                    ts[d].state = TS_LEFT; ts[d].at_end = 0;
                }
                break;
            case TS_LEFT:
                /* rbp drops whole glyphs at x2, so the ink test can only call the end once TS_ENDGAP px are empty.
                 * The first pass ends that way and records the real width; later passes stop TS_ENDPAD before x2. */
                if (!ts[d].at_end && ts[d].width > 0 && ts[d].offset >= ts[d].width - (TS_X2_NEW - TS_X1) + TS_ENDPAD)
                    ts[d].at_end = 2;
                if (ts[d].at_end == 1 && !ts[d].width) {
                    /* first pass: the end only shows once TS_ENDGAP px are empty (rbp drops the glyph at x2), i.e. up
                     * to ~40 px too far. The rightmost ink of that frame gives the exact width; ease back (TS_BACK) to
                     * the TS_ENDPAD stop that later passes use. */
                    ts[d].width = ts[d].ink_r >= 0 ? ts[d].ink_off + ts[d].ink_r - (ts_win_x[d] + TS_X1) + 1
                                                   : ts[d].ink_off + (TS_X2_NEW - TS_X1) - TS_ENDGAP;
                    if (ts[d].width <= TS_X2_NEW - TS_X1 - TS_ENDPAD) ts[d].is_long = 0;   /* fits after all */
                    else if (ts[d].offset > ts[d].width - (TS_X2_NEW - TS_X1) + TS_ENDPAD) {
                        ulog("uishim: title %d end by ink at offset %d, width %d: back to %d\n", d + 1, ts[d].offset,
                             ts[d].width, ts[d].width - (TS_X2_NEW - TS_X1) + TS_ENDPAD);
                        ts[d].state = TS_BACK; break;
                    }
                }
                if (ts[d].at_end || ts[d].offset > 4000) {
                    if (ts[d].at_end != 2) ulog("uishim: title %d end by %s at offset %d (est %d, width %d)\n", d + 1, ts[d].at_end == 2 ? "width" : "ink", ts[d].offset, ts[d].est, ts[d].width);
                    ts[d].state = TS_PAUSE_END; ts[d].t = now; break;
                }
                ts[d].offset += TS_STEP;
                break;
            case TS_BACK: {                            /* first pass overshot: step back to the end stop */
                int target = ts[d].width - (TS_X2_NEW - TS_X1) + TS_ENDPAD;
                ts[d].offset -= TS_STEP;
                if (ts[d].offset <= target) { ts[d].offset = target > 0 ? target : 0; ts[d].state = TS_PAUSE_END; ts[d].t = now; }
                break;
            }
            case TS_PAUSE_END:
                if (now - ts[d].t > TS_PAUSE_MS) ts[d].state = TS_RIGHT;
                break;
            case TS_RIGHT:
                ts[d].offset -= TS_STEP;
                if (ts[d].offset <= 0) { ts[d].offset = 0; ts[d].state = TS_IDLE; ts[d].t = now; }
                break;
            }
            if (ts[d].offset != prev) {                /* includes the last step back to rest */
                ts_set_x1(o, TS_X1 - ts[d].offset);
                ts_refresh(d);
            }
        }
    }
    return NULL;
}

/* flip side (rbp's display thread): measure / detect the end from the back buffer, cover the note strip */
static int ts_ink(void *prim, int x0, int w)
{
    void *p; int pitch, ink = 0;
    if (((lock_fn)SLOT(prim, SURF_Lock))(prim, 1 /* DSLF_READ */, &p, &pitch)) return -1;
    for (int y = TS_TXT_Y; y < TS_TXT_Y + TS_TXT_H && !ink; y++) {
        const uint16_t *row = (const uint16_t *)((uint8_t *)p + y * pitch);
        for (int x = x0; x < x0 + w; x++)
            if ((row[x] >> 11) > 14) { ink = 1; break; }       /* text is (224,224,224) on (32,32,32) */
    }
    ((unlock_fn)SLOT(prim, SURF_Unlock))(prim);
    return ink;
}

static int ts_ink_right(void *prim, int x0, int w)   /* rightmost title ink column in [x0, x0 + w), -1 = none */
{
    void *p; int pitch, r = -1;
    if (((lock_fn)SLOT(prim, SURF_Lock))(prim, 1 /* DSLF_READ */, &p, &pitch)) return -1;
    for (int y = TS_TXT_Y; y < TS_TXT_Y + TS_TXT_H; y++) {
        const uint16_t *row = (const uint16_t *)((uint8_t *)p + y * pitch);
        for (int x = x0 + w - 1; x > r && x >= x0; x--)
            if ((row[x] >> 11) > 14) { r = x; break; }
    }
    ((unlock_fn)SLOT(prim, SURF_Unlock))(prim);
    return r;
}

static void ts_flip(void *thiz)
{
    for (int d = 0; d < 2; d++) {
        int wx = ts_win_x[d], right = wx + TS_X2_NEW;
        if (!ts[d].x2_set) continue;
        if (ts[d].state == TS_IDLE && ts[d].offset == 0) {
            if (ts[d].need_measure && ms_now() - ts[d].t > 300) {   /* title drawn at rest by now */
                /* cut off = ink near the clip edge and the estimate is about as wide as the visible area */
                ts[d].is_long = ts_ink(thiz, right - TS_ENDGAP, TS_ENDGAP) == 1 && ts[d].est > (TS_X2_NEW - TS_X1) * 9 / 10;
                if (ts[d].patch) {                     /* note icon strip, for covering the text later */
                    struct { int x, y, w, h; } r = { wx + TS_PATCH_X, TS_ROW_Y, TS_PATCH_W, TS_ROW_H };
                    ((set_bflags_fn)SLOT(ts[d].patch, SURF_SetBlittingFlags))(ts[d].patch, 0);
                    ts[d].patch_ok = ((blit_fn)SLOT(ts[d].patch, SURF_Blit))(ts[d].patch, thiz, &r, 0, 0) == 0;
                }
                ts[d].need_measure = 0;
                /* exact width from rbp's own text measure (font 0, as ui_PlayInfo_SetTextStartScrollTimer uses for
                 * titles), so the first pass stops in the right place too. Called here, in rbp's drawing thread:
                 * it sets up rbp's shared font state. 0 / absurd = keep learning it from the ink. */
                int fw = ts[d].str ? ((int (*)(int, const uint16_t *, int))0x0018e640)(0, ts[d].str, 0) : 0;
                if (fw > 50 && fw < 4000) { ts[d].width = fw; if (fw <= TS_X2_NEW - TS_X1 - TS_ENDPAD) ts[d].is_long = 0; }
                ulog("uishim: title %d %s (est %d px, font width %d)\n", d + 1, ts[d].is_long ? "long: scrolls" : "fits", ts[d].est, fw);
            } else if (!ts[d].need_measure && ts[d].patch && ts[d].patch_ok && ms_now() - ts[d].t > 300) {
                /* keep the note-icon strip current while at rest (it turns into the tag tick when tagged) */
                struct { int x, y, w, h; } r = { wx + TS_PATCH_X, TS_ROW_Y, TS_PATCH_W, TS_ROW_H };
                ((set_bflags_fn)SLOT(ts[d].patch, SURF_SetBlittingFlags))(ts[d].patch, 0);
                ((blit_fn)SLOT(ts[d].patch, SURF_Blit))(ts[d].patch, thiz, &r, 0, 0);
            }
        } else {
            if (ts[d].state == TS_LEFT && ts[d].offset > 8 && ts_ink(thiz, right - TS_ENDGAP, TS_ENDGAP) == 0 && !ts[d].at_end) {
                ts[d].ink_off = ts[d].offset;          /* offset of the frame just checked */
                ts[d].ink_r = ts_ink_right(thiz, wx + TS_X1, TS_X2_NEW - TS_X1);   /* where the text really ends */
                ts[d].at_end = 1;
            }
            if (ts[d].patch_ok) blit(thiz, ts[d].patch, wx + TS_PATCH_X, TS_ROW_Y);
        }
    }
}

/* touch calibration targets (debug): while /tmp/uishim.cal exists, 9 crosshairs (x 80/640/1200, y 80/400/720) are
 * drawn over everything and every touch-down is logged ("uishim: cal touch x,y") and kept from rbp */
static const int cal_x[3] = { 80, 640, 1200 }, cal_y[3] = { 80, 400, 720 };
static void draw_cal(void *dst)
{
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            int x = cal_x[i], y = cal_y[j];
            fill(dst, 255, 255, 255, x - 30, y - 1, 61, 3);
            fill(dst, 255, 255, 255, x - 1, y - 30, 3, 61);
            fill(dst, 255, 40, 40, x - 3, y - 3, 7, 7);
        }
}
static int my_flip(void *thiz, const void *region, int flags)
{
    int mode = BROWSE_MODE;
    if (thiz == g_primary && strip_on && mode != 0 && g_dfb) {
        if (!g_deck) {
            render_all();
            g_deck = make_surface(thiz, &img_deck);
            g_row = make_surface(thiz, &img_row);
            g_stack = make_surface(thiz, &img_stack);
            g_tog[0] = make_surface(thiz, &img_tog[0]);
            g_tog[1] = make_surface(thiz, &img_tog[1]);
            g_pw[0] = make_surface(thiz, &img_pw[0]);
            for (int t = 0; t < 2; t++) g_flt[t] = make_surface(thiz, &img_flt[t]);
            render_search_controls();
            for (int t = 0; t < 4; t++) g_arw[t] = make_surface(thiz, &img_arw[t]);
            g_btag = make_surface(thiz, &img_btag);
            for (int t = 0; t < 2; t++) g_fxq[t] = make_surface(thiz, &img_fxq[t]);
            g_mci = make_surface(thiz, &img_mci);
            for (int t = 0; t < 6; t++) g_dk[t] = make_surface(thiz, &img_dk[t]);
            g_qoff = make_surface(thiz, &img_qoff);
            render_tag();
            for (int t = 0; t < 2; t++) g_tag[t] = make_surface(thiz, &img_tag[t]);
            g_bk[0] = make_surface(thiz, &img_bk[0]);
            {
                static uint32_t blank[TS_PATCH_W * TS_ROW_H];
                struct img pm = { TS_PATCH_W, TS_ROW_H, blank };
                for (int d = 0; d < 2; d++) ts[d].patch = make_surface(thiz, &pm);
            }
            for (int t = 0; t < 2; t++) g_msg[t] = make_surface(thiz, &img_msg[t]);
            render_eject();
            for (int sl = 0; sl < 2; sl++) for (int t = 0; t < EJ_N; t++) g_ej[sl][t] = make_surface(thiz, &img_ej[sl][t]);
            render_stuck();
            g_stuck[0] = make_surface(thiz, &img_stuck[0]);
            g_stuck[1] = make_surface(thiz, &img_stuck[1]);
            g_stuck[2] = make_surface(thiz, &img_stuck[2]);
            render_cfx();
            for (int t = 0; t < 6; t++) g_cfx[t] = make_surface_argb(&img_cfx[t]);
            render_cfxp();
            for (int t = 0; t < 6; t++) g_cfxp[t] = make_surface_argb(&img_cfxp[t]);
        }
        if (mode != last_mode) { last_mode = mode; stack_open = 0; }
        /* which screen is up, evaluated on every frame rbp presents (no forced redraws needed) */
        src_visible = window_shown(WIN_SOURCE_ID);
        if (src_visible) {                         /* which SOURCE row is highlighted: USB1 (y 145) or USB2 (y 236) */
            void *p; int pitch;
            if (((lock_fn)SLOT(thiz, SURF_Lock))(thiz, 1 /* DSLF_READ */, &p, &pitch) == 0) {
                const uint16_t *r1 = (const uint16_t *)((uint8_t *)p + 145 * pitch), *r2 = (const uint16_t *)((uint8_t *)p + 236 * pitch);
                if (px_is(r2, 300, 0, 125, 230)) ej_slot = 2;
                else if (px_is(r1, 300, 0, 125, 230)) ej_slot = 1;
                ((unlock_fn)SLOT(thiz, SURF_Unlock))(thiz);
            }
        }
        side_focus = 0;
        if (mode == MODE_BROWSE || mode == MODE_PLAYLIST) {
            void *p; int pitch;
            if (((lock_fn)SLOT(thiz, SURF_Lock))(thiz, 1 /* DSLF_READ */, &p, &pitch) == 0) {
                for (int y = 60; y < 690 && !side_focus; y += 8)
                    side_focus = px_is((const uint16_t *)((uint8_t *)p + y * pitch), 50, 0, 125, 230);
                if (mode == MODE_PLAYLIST) {
                    unsigned sum = 0;
                    for (int y = 8; y < 44; y++) {
                        const uint16_t *r = (const uint16_t *)((uint8_t *)p + y * pitch);
                        for (int x = 108; x < 330; x++) sum += r[x] * (unsigned)(x + y);
                    }
                    title_sig = sum; title_frames++;
                }
                ((unlock_fn)SLOT(thiz, SURF_Unlock))(thiz);
            }
        }
        util_visible = mode == 7 && window_shown(WIN_UTILITY_ID);  /* flag alone is 1 at startup; 7 = UTILITY */
        stuck_visible = src_visible && (restarting || (knobshim_usb1_stuck && knobshim_usb1_stuck()));
        ej_visible = !stuck_visible && src_visible && (usb_mounted || ej_state != EJ_IDLE);
        int fv = (mode == MODE_BROWSE || mode == MODE_PLAYLIST) && IsEnableFilterOnOff() && !window_shown(WIN_MENU_ID);
        if (fv != flt_visible) { flt_visible = fv; ulog("uishim: filter button %s (mode %d)\n", fv ? "shown" : "hidden", mode); }
        if (fv && g_flt[0]) {
            blit(thiz, g_flt[IsFilterOn() ? 1 : 0], FLT_X, FLT_Y);
            if (hold_ctrl == FLT_CTRL) draw_progress(thiz, FLT_X, FLT_Y, FLT_W, FLT_H);
            if (g_btag) blit(thiz, g_btag, BTAG_X, FLT_Y);
        }
        if (ej_visible && g_ej[ej_busy_slot - 1][ej_state]) blit(thiz, g_ej[ej_busy_slot - 1][ej_state], EJ_X, EJ_Y);
        if (stuck_visible && g_stuck[0]) {
            blit(thiz, g_stuck[restarting ? 1 : hold_ctrl == STUCK_CTRL ? 2 : 0], EJ_X, EJ_Y);
            if (hold_ctrl == STUCK_CTRL) draw_progress(thiz, EJ_X, EJ_Y, EJ_W, EJ_H);
        }
        if (deck_screen(mode)) {
            blit(thiz, g_deck, DECK_X, DECK_Y);
            if (mode == MODE_INFO && g_tag[0]) blit(thiz, g_tag[info_tagged(thiz)], TAG_X, TAG_Y);
            blit(thiz, g_bk[0], BK_DECK_X, DECK_Y);
            /* gap BACK -> deck row: rbp's header (e.g. the countdown's bar) shows through; black like the other gaps */
            fill(thiz, 0, 0, 0, BK_DECK_X + BK_DECK_W, DECK_Y, DECK_X - BK_DECK_X - BK_DECK_W, DECK_H);
            if (hold_ctrl == BTN_MENU) { int bx, bw; row_rect(BTN_MENU, DECK_X, DECK_W, DECK_GAP, &bx, &bw); draw_progress(thiz, bx, DECK_Y, bw, DECK_H); }
            ts_flip(thiz);                          /* title scroll: measure, cover the note strip */
            for (int d = 0; d < 2; d++)             /* TRACK SEARCH / SEARCH arrows */
                for (int dir = 0; dir < 2; dir++)
                    if (g_arw[0]) blit(thiz, g_arw[dir * 2 + (arw_id == ARW_CTRL + d * 2 + dir)],
                                       d * DK_DX + (dir ? ARW_RX : ARW_LX), ARW_Y);
            fx_visible = fx_panel_shown(thiz);
            if (fx_visible && g_mci && knobshim_master_cue) {
                if (knobshim_master_cue()) blit(thiz, g_mci, MCI_X, MCI_Y);
                else {                             /* off: paint the background back -- rbp does not redraw the panel's
                                                    * bottom margin (y 360-362, (32,32,32)) or the black strip below */
                    fill(thiz, 32, 32, 32, MCI_X, MCI_Y, MCI_W, 363 - MCI_Y);
                    fill(thiz, 0, 0, 0, MCI_X, 363, MCI_W, MCI_Y + MCI_H - 363);
                }
            }
            if (fx_visible && g_fxq[0]) blit(thiz, g_fxq[UiGetMixBeatFxQuantizeOn() ? 1 : 0], FXQB_X, FXQB_Y);
            if (fx_visible && hold_ctrl == TAP_CTRL) draw_progress(thiz, FXP_X, TAP_Y, FXP_W, TAP_H);
            for (int d = 0; d < 2; d++) {          /* VINYL / SLIP / MT buttons, QUANTIZE placeholder */
                int dx = d * DK_DX, slip = player_flag(d, 0xf59, 1), mt = player_flag(d, 0xe9a, 0xff),
                    q = player_flag(d, 0xedc, 0xff), vinyl = player_flag(d, 0xead, 0xff);
                if (slip < 0) continue;
                blit(thiz, g_dk[DK_VINYL * 2 + vinyl], dx + DK_VINYL_X, DK_BTN_Y);
                blit(thiz, g_dk[DK_SLIP * 2 + slip], dx + DK_SLIP_X, DK_BTN_Y);
                blit(thiz, g_dk[DK_MT * 2 + mt], dx + DK_MT_X, DK_BTN_Y);
                if (!q) blit(thiz, g_qoff, dx + DK_QOFF_X, DK_QOFF_Y);
            }
            int t = knobshim_cfx ? knobshim_cfx() : -1;
            if (t >= 0 && t < 6) { draw_cfx_cell(thiz, CFX_Y1, 0, t); draw_cfx_cell(thiz, CFX_Y2, 1, t); }
        }
        else if (mode != MODE_SEARCH) {
            blit(thiz, g_row, ROW_X0, BAR_Y);
            if (hold_ctrl == BTN_MENU) draw_progress(thiz, ROW_X0 + row_bx(BTN_MENU + 1), BAR_Y, ICON_W, BAR_H);
        }
        else {
            blit(thiz, g_tog[stack_open ? 1 : 0], TOG_X, TOG_Y);
            if (stack_open) {
                blit(thiz, g_stack, STACK_X, STACK_Y);
                if (hold_ctrl == BTN_MENU)         /* progress under the MENU item */
                    draw_progress(thiz, STACK_X, STACK_Y + BTN_MENU * (STACK_BH + STACK_GAP), STACK_W, STACK_BH);
            }
        }
        if (mode == MODE_SHORTCUT)                 /* MIXER MODE MIDI: dimmed (disabled) */
            fill_alpha(thiz, 0, 0, 0, 170, MIDI_X, MIDI_Y, MIDI_W, MIDI_H);
        if (util_visible && g_pw[0]) {             /* POWER OFF on the UTILITY screen */
            blit(thiz, g_pw[0], PW_X, PW_Y);
            if (hold_ctrl == PW_CTRL) draw_progress(thiz, PW_X, PW_Y, PW_W, PW_H);
        }
        if (hold_ctrl == REC_CTRL && rec_visible(mode)) draw_progress(thiz, REC_X, REC_Y, REC_W, REC_H);
        if (pw_state && g_msg[pw_state - 1])
            blit(thiz, g_msg[pw_state - 1], (1280 - MSG_W) / 2, (800 - MSG_H) / 2);
        if (cal_on) draw_cal(thiz);
    }
    return real_flip(thiz, region, flags);
}


static int in(int x, int y, int x0, int y0, int w, int h) { return x >= x0 && x < x0 + w && y >= y0 && y < y0 + h; }

static int row_hit(int x, int y, int x0, int y0, int w, int h, int gap)
{
    if (!in(x, y, x0, y0, w, h)) return -1;
    for (int b = 0; b < DECK_N; b++) {
        int bx, bw;
        deck_bx(b, w, gap, &bx, &bw);
        if (x - x0 >= bx && x - x0 < bx + bw) return b == BTN_SHORTCUT ? SC_CTRL : b;
    }
    return -1;                                     /* in a gap */
}

/* which control is at (x, y): 0..NBTN-1 = button, NBTN = toggle, -1 = none */
static int hit(int x, int y)
{
    int mode = BROWSE_MODE;
    if (!g_deck || !strip_on || mode == 0) return -1;
    if (y < 8) y = 8;                              /* screen's top edge belongs to the top bars (y 2/5 down): since
                                                    * the touch calibration a tap at the glass edge reads y 0-4 */
    if (util_visible && in(x, y, PW_X, PW_Y, PW_W, PW_H)) return PW_CTRL;
    if (flt_visible && in(x, y, FLT_X, FLT_Y, FLT_W, FLT_H)) return FLT_CTRL;
    if (flt_visible && in(x, y, BTAG_X, FLT_Y, FLT_W, FLT_H)) return BTAG_CTRL;
    if (mode == MODE_SHORTCUT && in(x, y, MIDI_X, MIDI_Y, MIDI_W, MIDI_H)) return BLOCK_CTRL;
    if (rec_visible(mode) && in(x, y, REC_X, REC_Y, REC_W, REC_H)) return REC_CTRL;
    if (deck_screen(mode)) {
        if (mode == MODE_INFO && in(x, y, TAG_X, TAG_Y, TAG_W, TAG_H)) return TAG_CTRL;
        for (int d = 0; d < 2; d++)
            for (int dir = 0; dir < 2; dir++)
                if (in(x, y, d * DK_DX + arw_tx[dir][0], ARW_TY, arw_tx[dir][1], ARW_TH)) return ARW_CTRL + d * 2 + dir;
        /* rbp's REMAIN/TIME touch zone reaches over the whole left of the deck info: swallow touches in the arrows'
         * dead zone and between the TRACK box and the time text (A.HOT CUE / AUTO CUE indicators, x 106-196), so the
         * time display only reacts on its text and digits (x 196+) */
        for (int d = 0; d < 2; d++)
            if (in(x, y, d * DK_DX + 54, ARW_TY, 10, ARW_TH) || in(x, y, d * DK_DX + 118, ARW_TY, 78, ARW_TH))
                return BLOCK_CTRL;
        if (knobshim_cfx_param && (in(x, y, CFX_X - 4, CFX_Y1 - 4, CFX_W + 8, CFX_H + 8) ||
                                   in(x, y, CFX_X - 4, CFX_Y2 - 4, CFX_W + 8, CFX_H + 8) ||
                                   in(x, y, CFXP_X0 - 2, CFX_Y1 + CFX_PY - 18, CFXP_W + 6, CFX_PH + 24) ||   /* bar + 18 px */
                                   in(x, y, CFXP_X0 - 2, CFX_Y2 + CFX_PY - 18, CFXP_W + 6, CFX_PH + 24))) return CFXP_CTRL;
        if (fx_visible && in(x, y, FXP_X, TAP_Y, FXP_W, TAP_H)) return TAP_CTRL;
        if (fx_visible && in(x, y, FXP_X, FXQ_Y, FXP_W, FXQ_H)) return FXQ_CTRL;
        for (int d = 0; d < 2; d++)
            for (int k = 0; k < DK_N; k++)
                if (in(x, y, dk_rect[k][0] + d * DK_DX, dk_rect[k][1], dk_rect[k][2], dk_rect[k][3]))
                    return DK_CTRL + d * DK_N + k;
        if (in(x, y, BK_DECK_X, DECK_Y, BK_DECK_W, DECK_H)) return BK_CTRL;
        return row_hit(x, y, DECK_X, DECK_Y, DECK_W, DECK_H, DECK_GAP);
    }
    if (mode != MODE_SEARCH) {
        if (!in(x, y, ROW_X0, BAR_Y, ROW_X1 - ROW_X0, BAR_H)) return -1;
        int b = 0;
        while (b < NROW && x - ROW_X0 >= row_bx(b) + row_w(b)) b++;
        if (b >= NROW || x - ROW_X0 < row_bx(b)) return -1;   /* in a gap */
        return b == 0 ? BK_CTRL : b - 1;
    }
    if (in(x, y, TOG_X, TOG_Y, TOG_W, TOG_H)) return NBTN;
    if (stack_open && in(x, y, STACK_X, STACK_Y, STACK_W, STACK_H)) {
        int b = (y - STACK_Y) / (STACK_BH + STACK_GAP);
        if (b >= STACK_N || (y - STACK_Y) - b * (STACK_BH + STACK_GAP) >= STACK_BH) return -1;
        return b;
    }
    return -1;
}

static void set_stack(int open)
{
    if (stack_open == open) return;
    stack_open = open;
    ui_com_draw_RefreshWinscape();                 /* rbp redraws; our Flip hook adds the new state */
}

/* called by touchshim-rx3 (screen pixels): returns 1 when the touch belongs to our controls.
 * A tap = down and up on the same control. Buttons send the RX3 key (press+release, global ch). */
int uishim_touch(int x, int y, int down)
{
    if (cal_on) {
        if (down == 1) ulog("uishim: cal touch %d,%d\n", x, y);
        return 1;
    }
    static int ej_pressed, stuck_pressed;
    if (down == 1 && getenv("UISHIM_TOUCH_LOG")) ulog("uishim: touch down %d,%d\n", x, y);
    if (stuck_visible && in(x, y, EJ_X, EJ_TY, EJ_W, EJ_TH)) {   /* hold STUCK_HOLD_MS: restart rbp */
        if (down == 1) ulog("uishim: USB1 STUCK button down (%d,%d) hold_ctrl %d restarting %d\n", x, y, hold_ctrl, restarting);
        if (down == 1 && !stuck_pressed && hold_ctrl < 0 && !restarting) {
            stuck_pressed = 1;
            hold_ms = STUCK_HOLD_MS; hold_t0 = ms_now(); hold_fired = 0; hold_ctrl = STUCK_CTRL;
            pthread_t t;
            if (pthread_create(&t, NULL, hold_thread, NULL) == 0) pthread_detach(t);
            ui_com_draw_RefreshWinscape();     /* show KEEP HOLDING now */
        } else if (!down) {
            if (hold_ctrl == STUCK_CTRL) { hold_ctrl = -1; ulog("uishim: USB1 STUCK released early\n"); ui_com_draw_RefreshWinscape(); }
            stuck_pressed = 0;
        }
        return 1;
    }
    if (!down && stuck_pressed) { if (hold_ctrl == STUCK_CTRL) hold_ctrl = -1; stuck_pressed = 0; return 1; }
    if (ej_visible && in(x, y, EJ_X, EJ_TY, EJ_W, EJ_TH)) {
        if (down) { ej_pressed = 1; return 1; }
        if (ej_pressed && (ej_state == EJ_IDLE || ej_state == EJ_BUSY) && usb_mounted) {
            ej_state = EJ_STOPPING;
            ui_com_draw_RefreshWinscape();
            pthread_t t;
            if (pthread_create(&t, NULL, eject_worker, NULL) == 0) pthread_detach(t);
            ulog("uishim: EJECT USB1\n");
        }
        ej_pressed = 0;
        return 1;
    }
    if (!down && ej_pressed) { ej_pressed = 0; return 1; }
    static int pressed = -1;
    static void (*send)(int, int, int, long);
    if (!send) send = (void (*)(int, int, int, long))dlsym(RTLD_DEFAULT, "knobshim_send_key");
    static void (*pset)(float);
    if (!pset) pset = (void (*)(float))dlsym(RTLD_DEFAULT, "knobshim_cfx_param_set");
    if (down == 2) {                               /* finger moved: only the CFX PARAMETER slider uses it */
        if (pressed == CFXP_CTRL && pset) {
            static int last_px = -1; static unsigned long long last_t;
            int px = x < CFXP_X0 ? CFXP_X0 : x > CFXP_X0 + CFXP_W ? CFXP_X0 + CFXP_W : x;
            if (px != last_px && ms_now() - last_t >= 30) {   /* <= ~33 updates/s */
                last_px = px; last_t = ms_now();
                pset((float)(px - CFXP_X0) / CFXP_W);
                ts_refresh(0);                     /* make rbp present a frame (bar redraw) */
            }
        }
        return 1;
    }
    if (pw_state) return 1;                        /* powering off: swallow all touches */
    if (down) {
        pressed = hit(x, y);
        if (pressed < 0) {
            if (stack_open) set_stack(0);          /* tap elsewhere closes the drop-down, and still reaches rbp */
            return 0;
        }
        if (pressed >= ARW_CTRL && pressed < ARW_CTRL + 4) { arw_down(pressed); return 1; }
        if (pressed == CFXP_CTRL) {                /* tap / start of a drag: jump to the finger */
            int px = x < CFXP_X0 ? CFXP_X0 : x > CFXP_X0 + CFXP_W ? CFXP_X0 + CFXP_W : x;
            if (pset) { pset((float)(px - CFXP_X0) / CFXP_W); ts_refresh(0); }
            return 1;
        }
        if (pressed == REC_CTRL && rec_active() && hold_ctrl < 0) {   /* recording: hold 1 s = stop */
            hold_ms = 1000; hold_t0 = ms_now(); hold_fired = 0; hold_ctrl = REC_CTRL;
            pthread_t t;
            if (pthread_create(&t, NULL, hold_thread, NULL) == 0) pthread_detach(t);
            return 1;
        }
        if ((pressed == PW_CTRL || pressed == BTN_MENU || pressed == FLT_CTRL || pressed == TAP_CTRL) && hold_ctrl < 0) {   /* hold controls */
            if (pressed == BTN_MENU && send) send(keys[BTN_MENU], 0, 1, 0);   /* MENU press now */
            hold_ms = pressed == FLT_CTRL || pressed == TAP_CTRL ? FLT_HOLD_MS : HOLD_MS;
            hold_t0 = ms_now();
            hold_fired = 0;
            hold_ctrl = pressed;
            pthread_t t;
            if (pthread_create(&t, NULL, hold_thread, NULL) == 0) pthread_detach(t);
        }
        return 1;
    }
    if (pressed >= ARW_CTRL && pressed < ARW_CTRL + 4) {   /* arrow release, wherever the finger is */
        arw_up(send);
        pressed = -1;
        return 1;
    }
    if (pressed == CFXP_CTRL) {                    /* slider released */
        pressed = -1;
        ulog("uishim: CFX PARAMETER %.2f\n", knobshim_cfx_param ? (double)knobshim_cfx_param() : -1.0);
        return 1;
    }
    if (pressed == REC_CTRL) {                     /* tap: start (idle) or TRACK MARK (recording) */
        int was_hold = hold_ctrl == REC_CTRL, fired = hold_fired;
        hold_ctrl = -1;
        if (!fired && send) {
            int key = was_hold ? K_TRACK_MARK : K_MASTER_REC;
            send(key, 0, 1, 0); usleep(80000); send(key, 2, 1, 0);
            ulog("uishim: %s\n", was_hold ? "TRACK MARK" : "MASTER REC (start)");
        }
        pressed = -1;
        return 1;
    }
    if (pressed == TAP_CTRL) {                     /* TAP release: a tap is TAP (held = AUTO, sent at 1 s) */
        if (!hold_fired && send) { hold_ctrl = -1; send(K_TAP, 0, 1, 0); usleep(80000); send(K_TAP, 2, 1, 0); }
        if (!hold_fired) ulog("uishim: TAP\n");
        hold_ctrl = -1;
        pressed = -1;
        return 1;
    }
    if (pressed == FLT_CTRL) {                     /* TRACK FILTER release: a tap toggles the filter */
        if (!hold_fired && send) {
            hold_ctrl = -1;
            send(K_TRACK_FILTER, 0, 1, 0);
            usleep(80000);
            send(K_TRACK_FILTER, 2, 1, 0);
            ulog("uishim: TRACK FILTER tap\n");
        }
        hold_ctrl = -1;
        pressed = -1;
        return 1;
    }
    if (pressed == PW_CTRL || pressed == BTN_MENU) {                     /* release of a hold control */
        if (pressed == BTN_MENU && send) {
            if (stack_open) set_stack(0);
            send(keys[BTN_MENU], 2, 1, 0);         /* MENU release: tap = MENU, after the long press = UTILITY */
            ulog("uishim: MENU %s\n", hold_fired ? "held (UTILITY)" : "tap");
        }
        hold_ctrl = -1;
        pressed = -1;
        return 1;
    }
    int b = hit(x, y);
    if (pressed >= 0 && b == pressed) {
        if (b == BLOCK_CTRL) ulog("uishim: touch blocked (%d,%d)\n", x, y);
        else if (b == NBTN) set_stack(!stack_open);
        else if (b == SC_CTRL) {
            if (send) { send(SC_KEY, 0, 1, 0); usleep(80000); send(SC_KEY, 2, 1, 0); }
            ulog("uishim: key 0x%04x (SHORTCUT)\n", SC_KEY);
        }
        else if (b == FXQ_CTRL) {
            int on = !UiGetMixBeatFxQuantizeOn();
            if (send) { send(K_FXQUANT, 0, 1, on); usleep(80000); send(K_FXQUANT, 2, 1, on); }
            ulog("uishim: Beat FX QUANTIZE %s\n", on ? "on" : "off");
        }
        else if (b == BTAG_CTRL) {
            if (send) { send(TAG_KEY, 0, 1, 0); usleep(80000); send(TAG_KEY, 2, 1, 0); }
            ulog("uishim: TAG TRACK (browse)\n");
        }
        else if (b == TAG_CTRL) {
            pthread_t t;
            if (pthread_create(&t, NULL, tag_worker, NULL) == 0) pthread_detach(t);
        }
        else if (b == BK_CTRL) {
            if (stack_open) set_stack(0);
            /* beyond the RX3: where BACK has no level to go up to, go to the deck view by pressing the screen's own
             * button again (each of these toggles back to the deck; UTILITY closes with a MENU tap) */
            int m = BROWSE_MODE, key = BK_KEY;
            switch (m) {
            case 4:  key = 0x0203; break;                       /* TAG LIST */
            case 5:  key = 0x020b; break;                       /* INFO */
            case 7:  key = 0x0206; break;                       /* UTILITY -> MENU tap */
            case 10: key = 0x0210; break;                       /* SHORTCUT */
            case 12: key = 0x0201; break;                       /* SOURCE */
            case MODE_BROWSE:   if (side_focus) key = 0x0202; break;
            case MODE_PLAYLIST: key = 0; break;                 /* back_playlist_worker decides */
            }
            if (!key) { pthread_t t; if (pthread_create(&t, NULL, back_playlist_worker, NULL) == 0) pthread_detach(t); }
            else if (send) { send(key, 0, 1, 0); usleep(80000); send(key, 2, 1, 0); }
            ulog("uishim: key 0x%04x (BACK, mode %d)\n", key, m);
        }
        else if (b >= DK_CTRL && b < DK_CTRL + 2 * DK_N) {   /* deck control: key on that deck's channel */
            int d = (b - DK_CTRL) / DK_N, k = (b - DK_CTRL) % DK_N;
            if (send) {
                send(dk_keys[k], 0, d + 1, 0);
                usleep(80000);
                send(dk_keys[k], 2, d + 1, 0);
            }
            ulog("uishim: deck %d %s\n", d + 1, dk_names[k]);
            usleep(50000);
            /* SLIP / MT / VINYL have no rbp indicator: make rbp present a frame so our button updates. Only that
             * deck's title row: a full RefreshWinscape on the deck screen does not repaint rbp's header (countdown
             * timer, INFO vanished until the next screen change). */
            ts_refresh(d);
        }
        else if (send) {
            if (stack_open) set_stack(0);
            send(keys[b], 0, 1, 0);
            usleep(80000);
            send(keys[b], 2, 1, 0);
            ulog("uishim: key 0x%04x (%s)\n", keys[b], labels[b]);
        }
    }
    pressed = -1;
    return 1;
}

static void *hook_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 600; i++) {               /* up to 60 s for rbp's display task to come up */
        usleep(100000);
        void *ds = *(void **)(uintptr_t)(DS_LAYER0_CTX + DS_CTX_PRIMARY);
        if (!ds) continue;
        void *prim = *(void **)((char *)ds + DS_SURF_IFACE);
        if (!prim || !g_dfb) continue;
        g_primary = prim;
        real_flip = (flip_fn)SLOT(prim, SURF_Flip);
        SLOT(prim, SURF_Flip) = (void *)my_flip;   /* one word: atomic for the DS thread */
        ulog("uishim: primary %p Flip %p hooked\n", prim, (void *)real_flip);
        pthread_t t2;
        if (pthread_create(&t2, NULL, eject_state_thread, NULL) == 0) pthread_detach(t2);
        if (!getenv("UISHIM_NO_TITLE_SCROLL") && pthread_create(&t2, NULL, title_thread, NULL) == 0) pthread_detach(t2);
        return NULL;
    }
    ulog("uishim: primary surface not found, no strip\n");
    return NULL;
}

/* rbp's MENU window (MY SETTINGS menu: LOAD / BACKGROUND COLOR / WAVEFORM COLOR) sits at x 712-1280,
 * y 0-466, so its title strip is under the top bar. Move it down by the bar height before rbp builds its
 * windows (layout record Obj_WS_BROWSER_Normal_WIN_MENU, type 17: [2] x1|y1<<16, [3] x2|y2<<16). */
#define WIN_MENU_REC ((uint32_t *)0x00526198)
#define MENU_SHIFT_Y 48
static void move_menu_window(void)
{
    uint32_t *r = WIN_MENU_REC;
    if (getenv("UISHIM_NO_MENU_MOVE")) { ulog("uishim: MENU window move disabled\n"); return; }
    if (r[0] != 17 || r[2] != 0x000002c8 || r[3] != 0x01d20500) {
        ulog("uishim: MENU window record not as expected (%08x %08x %08x), left alone\n", r[0], r[2], r[3]);
        return;
    }
    r[2] += MENU_SHIFT_Y << 16;
    r[3] += MENU_SHIFT_Y << 16;
    ulog("uishim: MENU window moved down %d px\n", MENU_SHIFT_Y);
}

/* Track Filter edit screen header: "TRACK FILTER" (text record type 0, x 116-280) and the match count "[n]"
 * (x 288-950). Title 6 px left (still clear of the USB1 box, which ends x 104) and the count right behind it, so both
 * end by x 325 and the top bar (x 334) fits; the count maxes out at "[>999]". Ink: title 111-252, count 259-325. */
#define FLT_TITLE_REC ((uint32_t *)0x0052f11c)
#define FLT_COUNT_REC ((uint32_t *)0x0052f0f0)
#define FLT_TITLE_SHIFT 6
#define FLT_COUNT_SHIFT 30
static void move_filter_header(void)
{
    uint32_t *t = FLT_TITLE_REC, *c = FLT_COUNT_REC;
    if (t[0] != 0 || t[2] != 0x000e0074 || t[3] != 0x00300118 || c[0] != 0 || c[2] != 0x000e0120 || c[3] != 0x003003b6) {
        ulog("uishim: filter header records not as expected (%08x %08x / %08x %08x), left alone\n", t[2], t[3], c[2], c[3]);
        return;
    }
    t[2] -= FLT_TITLE_SHIFT; t[3] -= FLT_TITLE_SHIFT;
    c[2] -= FLT_COUNT_SHIFT;
    ulog("uishim: filter header moved left (title %d, count %d px)\n", FLT_TITLE_SHIFT, FLT_COUNT_SHIFT);
}

/* Loop indicator in the deck boxes (loop icon + loop size image, rbp layout records Obj_CTRL_DECK_GRP_DECK_n_GRP_LOOP_
 * IMG_LOOPICON / LOOPSIZE; word 2 = x | y << 16, relative to the deck info window, screen y = rel + 40). The CFX
 * PARAMETER bar now takes the bottom of that row, so lift both images LOOP_LIFT px: the icon is then centred on
 * the same line as the CFX name (screen y ~233 / 454). Records are checked before patching. */
#define LOOP_LIFT 6
static void move_loop_indicator(void)
{
    static const struct { uintptr_t rec; uint32_t xy, img; } r[4] = {
        { 0x00538464, 0x00c0001e, 0x14e6 }, { 0x0053844c, 0x00bd0035, 0x149f },   /* deck 1: icon, size */
        { 0x00538820, 0x019d001e, 0x14e6 }, { 0x00538808, 0x019a0035, 0x149f },   /* deck 2 */
    };
    for (int i = 0; i < 4; i++) {
        uint32_t *w = (uint32_t *)r[i].rec;
        if (w[2] != r[i].xy || w[3] != r[i].img) {
            ulog("uishim: loop record %08x not as expected (%08x %08x), left alone\n", (unsigned)r[i].rec, w[2], w[3]);
            return;
        }
    }
    for (int i = 0; i < 4; i++) ((uint32_t *)r[i].rec)[2] -= (uint32_t)LOOP_LIFT << 16;
    ulog("uishim: loop indicator lifted %d px\n", LOOP_LIFT);
}

__attribute__((constructor)) static void uishim_init(void)
{
    if (!rbp_process_check()) return;
    if (getenv("UISHIM_OFF") && atoi(getenv("UISHIM_OFF"))) { ulog("uishim: off\n"); return; }
    move_menu_window();
    move_filter_header();
    if (!getenv("UISHIM_NO_LOOP_LIFT")) move_loop_indicator();
    nsf_load();
    pthread_t t;
    if (pthread_create(&t, NULL, hook_thread, NULL) == 0) pthread_detach(t);
    ulog("uishim: start\n");
}
