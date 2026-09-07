/* ui.c - ncurses front end: parameter rack, scope, spectrum, keyboard, steps */
#define _GNU_SOURCE
#include <curses.h>
#include <locale.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <termios.h>
#include "synth.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PANEL_W    46
/* A terminal never reports key releases, so a key that has stopped
 * repeating counts as released.  The window is wide until the first
 * auto-repeat arrives (the terminal's repeat delay, typically 500 ms),
 * then tightens once repeats are flowing. */
static double g_keyhold_first  = 0.62;
static double g_keyhold_repeat = 0.15;

void ui_set_keyhold(int ms)
{
    if (ms < 100) ms = 100;
    if (ms > 3000) ms = 3000;
    g_keyhold_first = ms / 1000.0;
    if (g_keyhold_repeat > g_keyhold_first * 0.5)
        g_keyhold_repeat = g_keyhold_first * 0.5;
}

enum {
    C_FRAME = 1, C_LABEL, C_VALUE, C_SEL, C_ACCENT, C_WARN, C_ALT, C_DIM, C_KEY
};

/* ---------------------------------------------------------------- */
static volatile sig_atomic_t g_quit;
static void on_sigint(int s) { (void)s; g_quit = 1; }

static double now_sec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* ---------------- computer keyboard -> note map ------------------ */
typedef struct { int ch; int semi; } KeyDef;

static const KeyDef g_keymap[] = {
    /* lower row: one octave starting at the current octave */
    {'z',0},{'s',1},{'x',2},{'d',3},{'c',4},{'v',5},{'g',6},{'b',7},
    {'h',8},{'n',9},{'j',10},{'m',11},{',',12},{'l',13},{'.',14},{';',15},{'/',16},
    /* upper row: one octave above */
    {'q',12},{'2',13},{'w',14},{'3',15},{'e',16},{'r',17},{'5',18},{'t',19},
    {'6',20},{'y',21},{'7',22},{'u',23},{'i',24},{'9',25},{'o',26},{'0',27},{'p',28},
};
#define NKEYS ((int)(sizeof(g_keymap) / sizeof(g_keymap[0])))

typedef struct {
    bool   on;
    bool   repeated;
    double last;
    int    note;
} KeyState;

/* ---------------- UI state --------------------------------------- */
typedef struct {
    Engine   *e;
    int       page;
    int       sel[PG_COUNT];
    int       scroll;
    int       octave;
    bool      latch;
    bool      help;
    int       preset;
    KeyState  keys[NKEYS];
    bool      down[128];      /* what the UI believes is sounding */
    char      msg[128];
    double    msgt;
    WavWriter *wav;
    char      wavname[128];
    uint32_t  rng;
    float     scopegain;
    /* text prompt */
    bool      prompting;
    char      prompt[64];
    char      input[96];
    int       inlen;
    int       promptmode;     /* 0 = save, 1 = load */
    double    escpend;        /* saw a lone ESC at this time, 0 = none */
    bool      escseq;         /* ...and a CSI/SS3 introducer after it   */
} UI;

static void say(UI *u, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(u->msg, sizeof u->msg, fmt, ap);
    va_end(ap);
    u->msgt = now_sec();
}

/* ---------------- drawing helpers -------------------------------- */
static void hbar(int y, int x, int w, float v, int pair)
{
    v = clampf(v, 0.0f, 1.0f);
    int on = (int)(v * w + 0.5f);
    attron(COLOR_PAIR(pair));
    for (int i = 0; i < w; i++)
        mvaddstr(y, x + i, i < on ? "▓" : "░");
    attroff(COLOR_PAIR(pair));
}

static void frame(int y, int x, int h, int w, const char *title)
{
    attron(COLOR_PAIR(C_FRAME));
    mvaddstr(y, x, "╭");
    for (int i = 1; i < w - 1; i++) mvaddstr(y, x + i, "─");
    mvaddstr(y, x + w - 1, "╮");
    for (int i = 1; i < h - 1; i++) {
        mvaddstr(y + i, x, "│");
        mvaddstr(y + i, x + w - 1, "│");
    }
    mvaddstr(y + h - 1, x, "╰");
    for (int i = 1; i < w - 1; i++) mvaddstr(y + h - 1, x + i, "─");
    mvaddstr(y + h - 1, x + w - 1, "╯");
    attroff(COLOR_PAIR(C_FRAME));
    if (title && *title) {
        attron(COLOR_PAIR(C_ACCENT) | A_BOLD);
        mvprintw(y, x + 2, " %s ", title);
        attroff(COLOR_PAIR(C_ACCENT) | A_BOLD);
    }
}

/* ---------------- radix-2 FFT for the spectrum ------------------- */
static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

/* ---------------- panels ----------------------------------------- */
static void draw_params(UI *u, int y0, int h)
{
    Engine *e = u->e;
    int x0 = 1;

    /* page tabs */
    int cx = x0 + 1;
    for (int p = 0; p < PG_COUNT; p++) {
        bool cur = (p == u->page);
        attron(cur ? (COLOR_PAIR(C_SEL) | A_BOLD) : COLOR_PAIR(C_DIM));
        mvprintw(y0, cx, " %s ", g_pagenames[p]);
        attroff(cur ? (COLOR_PAIR(C_SEL) | A_BOLD) : COLOR_PAIR(C_DIM));
        cx += (int)strlen(g_pagenames[p]) + 2;
        if (cx > PANEL_W - 4) break;
    }

    /* gather this page's params */
    int idx[64], n = 0;
    for (int i = 0; i < g_nparams && n < 64; i++)
        if (g_params[i].page == u->page) idx[n++] = i;

    int rows = h - 2;
    int sel  = u->sel[u->page];
    if (sel >= n) sel = u->sel[u->page] = n - 1;
    if (sel < 0)  sel = u->sel[u->page] = 0;

    int top = u->scroll;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    if (top < 0) top = 0;
    u->scroll = top;

    for (int r = 0; r < rows && top + r < n; r++) {
        const Param *pr = &g_params[idx[top + r]];
        int y = y0 + 2 + r;
        bool cur = (top + r == sel);
        char val[32];
        param_format(&e->patch, pr, val, sizeof val);

        if (cur) {
            attron(COLOR_PAIR(C_SEL));
            mvprintw(y, x0, " %-*s ", PANEL_W - 4, "");
            attroff(COLOR_PAIR(C_SEL));
        }
        attron(cur ? (COLOR_PAIR(C_SEL) | A_BOLD) : COLOR_PAIR(C_LABEL));
        mvprintw(y, x0 + 1, "%s%-14.14s", cur ? "▸" : " ", pr->name);
        attroff(cur ? (COLOR_PAIR(C_SEL) | A_BOLD) : COLOR_PAIR(C_LABEL));

        attron(cur ? (COLOR_PAIR(C_SEL) | A_BOLD) : COLOR_PAIR(C_VALUE));
        mvprintw(y, x0 + 17, "%10.10s", val);
        attroff(cur ? (COLOR_PAIR(C_SEL) | A_BOLD) : COLOR_PAIR(C_VALUE));

        hbar(y, x0 + 29, 14, param_norm(&e->patch, pr), cur ? C_ACCENT : C_DIM);
    }

    if (n > rows) {
        attron(COLOR_PAIR(C_DIM));
        mvprintw(y0 + 1, PANEL_W - 10, "%d/%d", sel + 1, n);
        attroff(COLOR_PAIR(C_DIM));
    }
}

static void draw_scope(UI *u, int y0, int x0, int h, int w)
{
    Engine *e = u->e;
    static const char *blocks[9] = { " ","▁","▂","▃","▄","▅","▆","▇","█" };

    int sh = h / 2;
    if (sh < 3) sh = 3;

    /* pull the most recent slice out of the audio thread's ring */
    enum { WIN = 2048, DISP = 900 };
    static float snap[WIN];
    unsigned wr = atomic_load(&e->scopew);
    for (int i = 0; i < WIN; i++)
        snap[i] = e->scope[(unsigned)(wr - WIN + (unsigned)i) & (SCOPE_LEN - 1)];

    int mid = y0 + 1 + sh / 2;

    /* auto-range like a scope's vertical knob: quick to open up, slow to
     * close down, so quiet patches stay visible without the trace jumping */
    float wpk = 0.0f;
    for (int i = WIN - DISP; i < WIN; i++)
        if (fabsf(snap[i]) > wpk) wpk = fabsf(snap[i]);
    float want = clampf(0.85f / (wpk > 0.004f ? wpk : 0.004f), 1.0f, 25.0f);
    u->scopegain += (want - u->scopegain) * (want < u->scopegain ? 0.02f : 0.25f);

    attron(COLOR_PAIR(C_DIM));
    mvaddstr(y0, x0, "scope");
    mvprintw(y0, x0 + w - 8, "x%-5.1f", (double)u->scopegain);
    attroff(COLOR_PAIR(C_DIM));

    /* trigger on a rising zero crossing so the waveform stands still */
    int trig = 0;
    for (int i = 1; i < WIN - DISP; i++)
        if (snap[i - 1] <= 0.0f && snap[i] > 0.0f) { trig = i; break; }

    attron(COLOR_PAIR(C_DIM));
    for (int x = 0; x < w; x++) mvaddstr(mid, x0 + x, "─");
    attroff(COLOR_PAIR(C_DIM));

    /* one vertical bar per column spanning that column's min..max */
    int half = sh / 2;
    attron(COLOR_PAIR(C_ACCENT) | A_BOLD);
    for (int x = 0; x < w; x++) {
        int a = trig + x * DISP / w;
        int b = trig + (x + 1) * DISP / w;
        if (b <= a) b = a + 1;
        if (b > WIN) b = WIN;
        float mn = 1e9f, mx = -1e9f;
        for (int i = a; i < b; i++) {
            float v = snap[i] * u->scopegain;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        int ytop = mid - (int)lrintf(clampf(mx, -1.0f, 1.0f) * (float)half);
        int ybot = mid - (int)lrintf(clampf(mn, -1.0f, 1.0f) * (float)half);
        if (ytop < y0 + 1)  ytop = y0 + 1;
        if (ybot > y0 + sh) ybot = y0 + sh;
        if (ytop == ybot) mvaddstr(ytop, x0 + x, "─");
        else for (int y = ytop; y <= ybot; y++) mvaddstr(y, x0 + x, "│");
    }
    attroff(COLOR_PAIR(C_ACCENT) | A_BOLD);

    /* ---- spectrum: 1024 point FFT, log frequency axis ---- */
    int fy = y0 + sh + 2;
    int fh = h - sh - 4;
    if (fh < 2) return;

    attron(COLOR_PAIR(C_DIM));
    mvaddstr(fy - 1, x0, "spectrum");
    attroff(COLOR_PAIR(C_DIM));

    enum { NF = 1024 };
    static float re[NF], im[NF], mag[NF / 2], smooth[512];
    for (int i = 0; i < NF; i++) {
        float win = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)i / (float)(NF - 1));
        re[i] = snap[WIN - NF + i] * win;
        im[i] = 0.0f;
    }
    fft(re, im, NF);
    for (int i = 0; i < NF / 2; i++)
        mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]) * (2.0f / NF);

    const float f_lo = 40.0f, f_hi = 16000.0f;
    const float binhz = e->sr / (float)NF;

    for (int x = 0; x < w && x < 512; x++) {
        float fa = f_lo * powf(f_hi / f_lo, (float)x / (float)w);
        float fb = f_lo * powf(f_hi / f_lo, (float)(x + 1) / (float)w);
        int   b0 = (int)(fa / binhz), b1 = (int)(fb / binhz);
        if (b0 < 1) b0 = 1;                 /* keep DC out of the display */
        if (b1 <= b0) b1 = b0 + 1;
        if (b1 > NF / 2) b1 = NF / 2;
        float m = 0.0f;
        for (int b = b0; b < b1; b++) if (mag[b] > m) m = mag[b];

        float db = 20.0f * log10f(m + 1e-7f);
        float v  = clampf((db + 72.0f) / 72.0f, 0.0f, 1.0f);
        smooth[x] = smooth[x] * 0.6f + v * 0.4f;
        v = smooth[x];

        int total = (int)(v * (float)(fh * 8));
        for (int r = 0; r < fh; r++) {
            int cell = total - (fh - 1 - r) * 8;
            cell = cell < 0 ? 0 : (cell > 8 ? 8 : cell);
            int pair = r < fh / 3 ? C_WARN : (r < fh * 2 / 3 ? C_VALUE : C_ALT);
            attron(COLOR_PAIR(pair));
            mvaddstr(fy + r, x0 + x, blocks[cell]);
            attroff(COLOR_PAIR(pair));
        }
    }

    /* frequency ruler */
    static const float ticks[] = { 100, 250, 500, 1000, 2500, 5000, 10000 };
    static const char *tlab[]  = { "100","250","500","1k","2.5k","5k","10k" };
    attron(COLOR_PAIR(C_DIM));
    for (int t = 0; t < 7; t++) {
        int x = (int)((logf(ticks[t] / f_lo) / logf(f_hi / f_lo)) * (float)w);
        if (x > 0 && x < w - 4) mvaddstr(fy + fh, x0 + x, tlab[t]);
    }
    attroff(COLOR_PAIR(C_DIM));
}

static void draw_keyboard(UI *u, int y, int x0, int w)
{
    static const int white[7]    = { 0, 2, 4, 5, 7, 9, 11 };
    static const int blackaft[5] = { 0, 1, 3, 4, 5 };   /* black key after white i */

    int octaves = 3;                       /* the key map spans C .. E two up */
    while (octaves > 1 && octaves * 21 > w) octaves--;
    int total = octaves * 21;
    int ox0   = x0 + (w - total) / 2;
    int start = u->octave * 12;

    for (int o = 0; o < octaves; o++) {
        int ox = ox0 + o * 21;
        for (int i = 0; i < 7; i++) {
            int  note = start + o * 12 + white[i];
            bool on   = note >= 0 && note < 128 && u->down[note];
            int  kx   = ox + i * 3;
            /* shade vs solid, not just colour, so held keys still read on a
             * monochrome terminal */
            attron(on ? (COLOR_PAIR(C_KEY) | A_BOLD) : COLOR_PAIR(C_LABEL));
            mvaddstr(y + 1, kx, on ? "██" : "▓▓");
            attroff(on ? (COLOR_PAIR(C_KEY) | A_BOLD) : COLOR_PAIR(C_LABEL));
            if (i == 0) {
                char nb[8];
                note_name(note, nb, sizeof nb);
                attron(COLOR_PAIR(C_DIM));
                mvaddstr(y + 2, kx, nb);
                attroff(COLOR_PAIR(C_DIM));
            }
        }
        for (int i = 0; i < 5; i++) {
            int  wi   = blackaft[i];
            int  note = start + o * 12 + white[wi] + 1;
            bool on   = note >= 0 && note < 128 && u->down[note];
            int  kx   = ox + wi * 3 + 2;
            attron(on ? (COLOR_PAIR(C_ALT) | A_BOLD) : COLOR_PAIR(C_DIM));
            mvaddstr(y,     kx, on ? "█" : "▄");
            mvaddstr(y + 1, kx, on ? "█" : "▀");
            attroff(on ? (COLOR_PAIR(C_ALT) | A_BOLD) : COLOR_PAIR(C_DIM));
        }
    }
}

static void draw_seq(UI *u, int y, int x0, int w)
{
    Engine *e = u->e;
    int cur = atomic_load(&e->uistep);
    char nb[8];

    attron(COLOR_PAIR(C_DIM));
    mvaddstr(y, x0, "seq");
    attroff(COLOR_PAIR(C_DIM));

    int cellw = 5;
    int max = (w - 6) / cellw;
    if (max > NSTEPS) max = NSTEPS;

    for (int i = 0; i < max; i++) {
        int x = x0 + 4 + i * cellw;
        bool playing = e->playing && e->patch.seqon && i == cur;
        bool rec     = e->recording && i == e->recstep;
        bool inlen   = i < e->patch.seqlen;

        int pair = playing ? C_SEL : (rec ? C_WARN : (inlen ? C_LABEL : C_DIM));
        int attr = COLOR_PAIR(pair) | (playing || rec ? A_BOLD : 0);
        if (playing || rec) attr |= A_REVERSE;

        attron(attr);
        if (e->seq[i].note >= 0) {
            note_name(e->seq[i].note, nb, sizeof nb);
            mvprintw(y, x, "%-4.4s", nb);
        } else {
            mvprintw(y, x, "%-4.4s", inlen ? " · " : "   ");
        }
        attroff(attr);
    }
}

static void draw_header(UI *u, int w)
{
    Engine *e = u->e;
    char nb[8];

    attron(COLOR_PAIR(C_SEL) | A_BOLD);
    for (int i = 0; i < w; i++) mvaddch(0, i, ' ');
    mvprintw(0, 1, " %s %s ", APP_NAME, APP_VERSION);
    attroff(COLOR_PAIR(C_SEL) | A_BOLD);

    attron(COLOR_PAIR(C_VALUE) | A_BOLD);
    mvprintw(0, 16, "%s", e->patch.name);
    attroff(COLOR_PAIR(C_VALUE) | A_BOLD);

    int x = 40;
    attron(COLOR_PAIR(C_LABEL));
    mvprintw(0, x, "oct"); x += 4;
    attroff(COLOR_PAIR(C_LABEL));
    attron(COLOR_PAIR(C_ACCENT));
    mvprintw(0, x, "%s", note_name(u->octave * 12, nb, sizeof nb)); x += 5;
    attroff(COLOR_PAIR(C_ACCENT));

    attron(COLOR_PAIR(C_LABEL)); mvprintw(0, x, "voices"); attroff(COLOR_PAIR(C_LABEL));
    attron(COLOR_PAIR(C_ACCENT));
    mvprintw(0, x + 7, "%2d/%-2d", atomic_load(&e->nactive), e->patch.maxvoices);
    attroff(COLOR_PAIR(C_ACCENT));
    x += 14;

    attron(COLOR_PAIR(C_LABEL)); mvprintw(0, x, "cpu"); attroff(COLOR_PAIR(C_LABEL));
    attron(COLOR_PAIR(C_ACCENT)); mvprintw(0, x + 4, "%3.0f%%", (double)atomic_load(&e->cpu));
    attroff(COLOR_PAIR(C_ACCENT));
    x += 10;

    if (u->latch) {
        attron(COLOR_PAIR(C_ALT) | A_BOLD); mvprintw(0, x, "LATCH");
        attroff(COLOR_PAIR(C_ALT) | A_BOLD);
    }
    x += 6;
    if (e->patch.arpon) {
        attron(COLOR_PAIR(C_ACCENT) | A_BOLD); mvprintw(0, x, "ARP");
        attroff(COLOR_PAIR(C_ACCENT) | A_BOLD);
    }
    x += 4;
    if (e->playing) {
        attron(COLOR_PAIR(C_ACCENT) | A_BOLD); mvprintw(0, x, "▶");
        attroff(COLOR_PAIR(C_ACCENT) | A_BOLD);
    }
    x += 2;
    if (e->recording) {
        attron(COLOR_PAIR(C_WARN) | A_BOLD); mvprintw(0, x, "STEP-REC");
        attroff(COLOR_PAIR(C_WARN) | A_BOLD);
    }
    x += 9;
    if (u->wav) {
        attron(COLOR_PAIR(C_WARN) | A_BOLD);
        mvprintw(0, x, "● WAV %.1fs", (double)wav_frames(u->wav) / e->sr);
        attroff(COLOR_PAIR(C_WARN) | A_BOLD);
    }

    /* second row: audio device on the left, output meters on the right */
    attron(COLOR_PAIR(C_DIM));
    mvprintw(1, 2, "%.*s", w - 30, audio_info());
    attroff(COLOR_PAIR(C_DIM));

    float pl = atomic_load(&e->peakl), prr = atomic_load(&e->peakr);
    int mx = w - 24;
    if (mx > 20) {
        attron(COLOR_PAIR(C_LABEL));
        mvaddstr(1, mx, "L"); mvaddstr(1, mx + 12, "R");
        attroff(COLOR_PAIR(C_LABEL));
        hbar(1, mx + 2,  9, pl,  pl  > 0.95f ? C_WARN : C_ACCENT);
        hbar(1, mx + 14, 9, prr, prr > 0.95f ? C_WARN : C_ACCENT);
    }
}

static void draw_help(int rows, int cols)
{
    static const char *lines[] = {
"  PLAYING",
"    z s x d c v g b h n j m , l . ; /   lower octave (white + black keys)",
"    q 2 w 3 e r 5 t 6 y 7 u i 9 o 0 p   upper octave",
"    [ ]        octave down / up            Enter  latch held notes",
"    1          all notes off (panic)       4      arpeggiator on/off",
"",
"  EDITING",
"    Tab / BTab   next / previous page      Up Down   select parameter",
"    Left Right   change value              Shift+LR  fine change",
"    - =          master volume             \\        randomize patch",
"    '            reset patch to Init       PgUp/PgDn  next / prev preset",
"    F9 save patch    F10 load patch",
"",
"  SEQUENCER",
"    Space      start / stop transport      8 / F4 / ^T  step record on/off",
"    while step recording, each key you play writes a step and advances",
"    Backspace  write a rest                Delete   clear the sequence",
"",
"  RECORDING",
"    F5 or ^R   start / stop writing a .wav file in the current directory",
"",
"  F1 or ?  toggle this help        F12 or Ctrl-C  quit",
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    int w = 78, h = n + 4;
    if (w > cols - 2) w = cols - 2;
    if (h > rows - 2) h = rows - 2;
    int x = (cols - w) / 2, y = (rows - h) / 2;

    for (int i = 0; i < h; i++) {
        attron(COLOR_PAIR(C_LABEL));
        mvprintw(y + i, x, "%*s", w, "");
        attroff(COLOR_PAIR(C_LABEL));
    }
    frame(y, x, h, w, "keys");
    for (int i = 0; i < n && i + 2 < h - 1; i++) {
        bool head = lines[i][0] && lines[i][2] != ' ' && lines[i][2] != '\0';
        attron(head ? (COLOR_PAIR(C_ACCENT) | A_BOLD) : COLOR_PAIR(C_LABEL));
        mvprintw(y + 2 + i, x + 2, "%-*.*s", w - 4, w - 4, lines[i]);
        attroff(head ? (COLOR_PAIR(C_ACCENT) | A_BOLD) : COLOR_PAIR(C_LABEL));
    }
}

/* ---------------- note plumbing ---------------------------------- */
static void note_on(UI *u, int note)
{
    if (note < 0 || note > 127 || u->down[note]) return;
    u->down[note] = true;
    engine_push(u->e, EV_ON, note, 0.85f);
}

static void note_off(UI *u, int note)
{
    if (note < 0 || note > 127 || !u->down[note]) return;
    u->down[note] = false;
    engine_push(u->e, EV_OFF, note, 0.0f);
}

static void all_notes_off(UI *u)
{
    for (int i = 0; i < 128; i++) u->down[i] = false;
    for (int i = 0; i < NKEYS; i++) u->keys[i].on = false;
    engine_push(u->e, EV_ALLOFF, 0, 0);
}

static void record_step(UI *u, int note)
{
    Engine *e = u->e;
    e->seq[e->recstep].note = note;
    e->seq[e->recstep].vel  = 0.85f;
    e->seq[e->recstep].tie  = false;
    e->recstep = (e->recstep + 1) % (e->patch.seqlen < 1 ? 1 : e->patch.seqlen);
}

/* ---------------- wav capture ------------------------------------ */
static void wav_toggle(UI *u)
{
    Engine *e = u->e;
    if (u->wav) {
        atomic_store(&e->capturing, false);
        wav_close(u->wav);
        u->wav = NULL;
        say(u, "stopped recording -> %s", u->wavname);
        return;
    }
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(u->wavname, sizeof u->wavname, "synthc-%04d%02d%02d-%02d%02d%02d.wav",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    u->wav = wav_open(u->wavname, (int)e->sr);
    if (!u->wav) { say(u, "cannot open %s", u->wavname); return; }
    atomic_store(&e->recr, atomic_load(&e->recw));
    atomic_store(&e->capturing, true);
    say(u, "recording to %s", u->wavname);
}

static void wav_pump(UI *u)
{
    Engine *e = u->e;
    if (!u->wav) return;
    unsigned w = atomic_load_explicit(&e->recw, memory_order_acquire);
    unsigned r = atomic_load_explicit(&e->recr, memory_order_relaxed);
    float tmp[2048];
    int n = 0;
    while (r != w && n < 2048) { tmp[n++] = e->rec[r]; r = (r + 1) & (RECRING - 1); }
    atomic_store_explicit(&e->recr, r, memory_order_release);
    if (n >= 2) wav_write(u->wav, tmp, n / 2);
}

/* ---------------- input ------------------------------------------ */
static void handle_prompt(UI *u, int ch)
{
    Engine *e = u->e;
    if (ch == 27) { u->prompting = false; say(u, "cancelled"); return; }
    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        u->prompting = false;
        if (u->inlen == 0) { say(u, "cancelled"); return; }
        if (u->promptmode == 0) {
            if (patch_save_file(&e->patch, u->input) == 0) say(u, "saved %s", u->input);
            else say(u, "could not write %s", u->input);
        } else {
            if (patch_load_file(&e->patch, u->input) == 0) {
                engine_reset_fx(e);
                say(u, "loaded %s", u->input);
            } else say(u, "could not read %s", u->input);
        }
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (u->inlen > 0) u->input[--u->inlen] = 0;
        return;
    }
    if (ch >= 32 && ch < 127 && u->inlen < (int)sizeof(u->input) - 1) {
        u->input[u->inlen++] = (char)ch;
        u->input[u->inlen] = 0;
    }
}

static void start_prompt(UI *u, int mode)
{
    u->prompting = true;
    u->promptmode = mode;
    snprintf(u->prompt, sizeof u->prompt, mode == 0 ? "save patch as" : "load patch from");
    char safe[32];
    snprintf(safe, sizeof safe, "%s", u->e->patch.name);
    for (char *c = safe; *c; c++) if (*c == ' ' || *c == '/') *c = '-';
    snprintf(u->input, sizeof u->input, "%s.patch", safe);
    u->inlen = (int)strlen(u->input);
}

static const Param *cur_param(UI *u)
{
    int n = 0;
    for (int i = 0; i < g_nparams; i++)
        if (g_params[i].page == u->page) {
            if (n == u->sel[u->page]) return &g_params[i];
            n++;
        }
    return NULL;
}

static int page_count(UI *u)
{
    int n = 0;
    for (int i = 0; i < g_nparams; i++) if (g_params[i].page == u->page) n++;
    return n;
}

static bool handle_key(UI *u, int ch, double now)
{
    Engine *e = u->e;

    if (u->prompting) { handle_prompt(u, ch); return true; }

    /* piano keys first */
    for (int i = 0; i < NKEYS; i++) {
        if (g_keymap[i].ch != ch) continue;
        KeyState *k = &u->keys[i];
        int note = u->octave * 12 + g_keymap[i].semi;
        if (note < 0 || note > 127) return true;

        if (u->latch) {
            if (now - k->last < 0.35) { k->last = now; return true; } /* debounce repeat */
            k->last = now;
            if (u->down[note]) {
                note_off(u, note);
            } else {
                note_on(u, note);
                if (e->recording) record_step(u, note);
            }
            return true;
        }
        if (!k->on) {
            k->on = true;
            k->repeated = false;
            k->note = note;
            note_on(u, note);
            if (e->recording) record_step(u, note);
        } else {
            k->repeated = true;
        }
        k->last = now;
        return true;
    }

    switch (ch) {
    case '\t': u->page = (u->page + 1) % PG_COUNT; u->scroll = 0; return true;
    case KEY_BTAB: u->page = (u->page + PG_COUNT - 1) % PG_COUNT; u->scroll = 0; return true;

    case KEY_UP:
        if (u->sel[u->page] > 0) u->sel[u->page]--;
        return true;
    case KEY_DOWN:
        if (u->sel[u->page] < page_count(u) - 1) u->sel[u->page]++;
        return true;

    case KEY_LEFT:  case KEY_RIGHT:
    case KEY_SLEFT: case KEY_SRIGHT: {
        const Param *pr = cur_param(u);
        if (!pr) return true;
        int dir = (ch == KEY_RIGHT || ch == KEY_SRIGHT) ? 1 : -1;
        bool fine = (ch == KEY_SLEFT || ch == KEY_SRIGHT);
        param_nudge(&e->patch, pr, dir, fine);
        return true;
    }

    case '[': if (u->octave > 0) { all_notes_off(u); u->octave--; } return true;
    case ']': if (u->octave < 8) { all_notes_off(u); u->octave++; } return true;

    case '-': e->patch.volume = clampf(e->patch.volume - 0.05f, 0, 1.5f);
              say(u, "volume %.0f%%", e->patch.volume * 100.0f); return true;
    case '=': e->patch.volume = clampf(e->patch.volume + 0.05f, 0, 1.5f);
              say(u, "volume %.0f%%", e->patch.volume * 100.0f); return true;

    case ' ':
        e->playing = !e->playing;
        if (e->playing) { e->step = -1; e->seqclk = 0; e->patch.seqon = 1; }
        else all_notes_off(u);
        say(u, e->playing ? "transport running" : "transport stopped");
        return true;

    case '\n': case '\r': case KEY_ENTER:
        u->latch = !u->latch;
        if (!u->latch) all_notes_off(u);
        say(u, u->latch ? "latch on" : "latch off");
        return true;

    /* '2' '3' '5' '6' '7' '9' '0' are piano keys, so the shortcuts below
     * only use digits the key map does not claim, plus function and
     * control keys, which never collide with playing. */
    case '1': case KEY_F(2): case 16:      /* Ctrl-P */
        all_notes_off(u); engine_push(e, EV_PANIC, 0, 0); say(u, "panic"); return true;
    case '4': case KEY_F(3): case 1:       /* Ctrl-A */
        e->patch.arpon = !e->patch.arpon;
        all_notes_off(u);
        say(u, e->patch.arpon ? "arp on" : "arp off");
        return true;
    case '8': case KEY_F(4): case 20:      /* Ctrl-T */
        e->recording = !e->recording;
        e->recstep = 0;
        say(u, e->recording ? "step record: play notes to fill steps" : "step record off");
        return true;
    case KEY_F(5): case 18:                /* Ctrl-R */
        wav_toggle(u); return true;

    case KEY_BACKSPACE: case 127: case 8:
        if (e->recording) {
            e->seq[e->recstep].note = -1;
            e->recstep = (e->recstep + 1) % (e->patch.seqlen < 1 ? 1 : e->patch.seqlen);
        }
        return true;
    case KEY_DC:
        for (int i = 0; i < NSTEPS; i++) e->seq[i].note = -1;
        e->recstep = 0;
        say(u, "sequence cleared");
        return true;

    case '\\': patch_randomize(&e->patch, &u->rng); engine_reset_fx(e);
               say(u, "randomized"); return true;
    case '\'': patch_init(&e->patch); engine_reset_fx(e); say(u, "patch reset"); return true;

    case KEY_NPAGE:
        u->preset = (u->preset + 1) % patch_preset_count();
        patch_load_preset(&e->patch, u->preset); engine_reset_fx(e);
        say(u, "preset: %s", e->patch.name); return true;
    case KEY_PPAGE:
        u->preset = (u->preset + patch_preset_count() - 1) % patch_preset_count();
        patch_load_preset(&e->patch, u->preset); engine_reset_fx(e);
        say(u, "preset: %s", e->patch.name); return true;

    case KEY_F(9):  start_prompt(u, 0); return true;
    case KEY_F(10): start_prompt(u, 1); return true;

    case '?': case KEY_F(1): u->help = !u->help; return true;

    case KEY_F(12): case 17: return false;   /* 17 = Ctrl-Q */
    case KEY_RESIZE: clear(); return true;
    }
    return true;
}

/* ---------------- main loop -------------------------------------- */
int ui_run(Engine *e)
{
    UI u;
    memset(&u, 0, sizeof u);
    u.e = e;
    u.octave = 4;
    u.rng = (uint32_t)time(NULL) | 1u;
    u.scopegain = 1.0f;
    snprintf(u.msg, sizeof u.msg, "F1 or ? for help   z x c v b / q w e r t = notes   "
                                  "[ ] octave   tab = page");
    u.msgt = now_sec();

    setlocale(LC_ALL, "");
    initscr();
    cbreak();

    /* Ctrl-S / Ctrl-Q are XON/XOFF by default, which would freeze the
     * terminal and swallow our quit key.  Turn software flow control off. */
    struct termios tio;
    if (tcgetattr(0, &tio) == 0) {
        tio.c_iflag &= (tcflag_t)~(IXON | IXOFF | IXANY);
        tcsetattr(0, TCSANOW, &tio);
    }

    noecho();
    nonl();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    set_escdelay(50);   /* long enough that a slow link cannot split an
                         * escape sequence and leak its '[' as an octave key */

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_FRAME,  COLOR_CYAN,    -1);
        init_pair(C_LABEL,  COLOR_WHITE,   -1);
        init_pair(C_VALUE,  COLOR_YELLOW,  -1);
        init_pair(C_SEL,    COLOR_BLACK,   COLOR_CYAN);
        init_pair(C_ACCENT, COLOR_GREEN,   -1);
        init_pair(C_WARN,   COLOR_RED,     -1);
        init_pair(C_ALT,    COLOR_MAGENTA, -1);
        init_pair(C_DIM,    COLOR_BLUE,    -1);
        init_pair(C_KEY,    COLOR_CYAN,    -1);
    }

    signal(SIGINT, on_sigint);

    bool running = true;
    while (running && !g_quit) {
        double now = now_sec();

        /* A slow link can split an escape sequence across reads, and once
         * ncurses times out on the ESC it hands us the tail as plain keys -
         * turning an arrow key into '[' plus a letter, which would jump the
         * octave and play a note.  Swallow the remains of a broken sequence
         * instead: losing the odd keypress beats acting on the wrong one. */
        int ch;
        while ((ch = getch()) != ERR) {
            if (!u.prompting) {
                if (ch == 27) { u.escpend = now; u.escseq = false; continue; }
                if (u.escpend > 0.0) {
                    if (now - u.escpend > 0.15) {
                        u.escpend = 0.0;
                        u.escseq = false;
                    } else if (!u.escseq && (ch == '[' || ch == 'O')) {
                        u.escseq = true; u.escpend = now; continue;
                    } else if (u.escseq) {
                        u.escpend = now;
                        if (ch >= 0x40 && ch <= 0x7e) { u.escseq = false; u.escpend = 0.0; }
                        continue;
                    } else {
                        u.escpend = 0.0;   /* ESC then an ordinary key */
                    }
                }
            }
            if (!handle_key(&u, ch, now)) { running = false; break; }
        }

        if (!u.latch) {
            for (int i = 0; i < NKEYS; i++) {
                KeyState *k = &u.keys[i];
                if (!k->on) continue;
                double limit = k->repeated ? g_keyhold_repeat : g_keyhold_first;
                if (now - k->last > limit) { k->on = false; note_off(&u, k->note); }
            }
        }

        wav_pump(&u);

        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        erase();

        if (rows < 22 || cols < 80) {
            mvprintw(0, 0, "terminal too small: %dx%d, need at least 80x22", cols, rows);
            refresh();
            napms(60);
            continue;
        }

        draw_header(&u, cols);

        int body_y = 2;
        int body_h = rows - 8 - body_y;
        if (body_h < 6) body_h = 6;

        frame(body_y, 0, body_h, PANEL_W, "patch");
        draw_params(&u, body_y + 1, body_h - 2);

        frame(body_y, PANEL_W, body_h, cols - PANEL_W, "output");
        draw_scope(&u, body_y + 1, PANEL_W + 2, body_h - 2, cols - PANEL_W - 4);

        draw_keyboard(&u, rows - 6, 2, cols - 4);
        draw_seq(&u, rows - 3, 2, cols - 4);

        if (u.prompting) {
            attron(COLOR_PAIR(C_SEL) | A_BOLD);
            mvprintw(rows - 2, 0, "%*s", cols, "");
            mvprintw(rows - 2, 1, " %s: %s_ (Enter ok, Esc cancel)", u.prompt, u.input);
            attroff(COLOR_PAIR(C_SEL) | A_BOLD);
        } else if (now - u.msgt < 4.0) {
            attron(COLOR_PAIR(C_ACCENT));
            mvprintw(rows - 2, 2, "%.*s", cols - 4, u.msg);
            attroff(COLOR_PAIR(C_ACCENT));
        }

        attron(COLOR_PAIR(C_DIM));
        mvprintw(rows - 1, 2, "%.*s", cols - 4,
                 "F1 help  tab page  ↑↓ select  ←→ edit  "
                 "[ ] octave  space play  enter latch  4 arp  8 step-rec  F5 wav  F12 quit");
        attroff(COLOR_PAIR(C_DIM));

        if (u.help) draw_help(rows, cols);

        refresh();
        napms(16);
    }

    if (u.wav) { atomic_store(&e->capturing, false); wav_pump(&u); wav_close(u.wav); }
    endwin();
    if (u.wavname[0]) printf("wrote %s\n", u.wavname);
    return 0;
}
