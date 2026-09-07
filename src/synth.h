/* synthc - a polyphonic subtractive synthesizer for the terminal */
#ifndef SYNTH_H
#define SYNTH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>

#define APP_NAME     "synthc"
#define APP_VERSION  "1.0"

#define MAX_VOICES   16
#define SCOPE_LEN    4096
#define NSTEPS       16
#define EVQ_SIZE     512
#define RECRING      (1 << 19)

/* ------------------------------------------------------------------ *
 *  Patch: everything the user can tweak.  Serialized by params.c.
 * ------------------------------------------------------------------ */
typedef struct {
    char  name[32];

    /* oscillators */
    int   o1wave, o2wave;
    float oscmix;          /* 0 = osc1 only .. 1 = osc2 only */
    float detune;          /* cents */
    int   o2oct;           /* -2..+2 */
    float pw;              /* pulse width 0.02..0.98 */
    float sublevel;        /* square one octave down */
    float noiselevel;
    float glide;           /* portamento, ms */
    float spread;          /* stereo spread of voices */
    int   maxvoices;

    /* filter */
    float cutoff;          /* Hz */
    float res;             /* 0..1 */
    int   fmode;
    float fenvamt;         /* -1..1 -> +-6 octaves */
    float keytrack;        /* 0..1 */
    float drive;           /* 1..8 */

    /* envelopes (seconds) */
    float aA, aD, aS, aR;
    float fA, fD, fS, fR;

    /* lfo */
    int   lwave;
    float lrate;           /* Hz */
    float l2pitch;         /* cents */
    float l2cut;           /* octaves */
    float l2pw;
    float l2amp;

    /* effects */
    float sat;             /* pre-fx saturation drive */
    float chdepth, chrate, chmix;
    int   dlsync, dldiv;
    float dltime, dlfb, dldamp, dlmix;
    int   dlping;
    float rvsize, rvdamp, rvmix, rvwidth;

    /* arpeggiator / sequencer / clock */
    int   arpon, arpmode, arpdiv, arpoct;
    float arpgate;
    int   seqon, seqlen;
    float swing;
    float bpm;

    /* master */
    float volume;          /* linear 0..1.5 */
    int   transpose;       /* semitones */
} Patch;

/* ------------------------------------------------------------------ *
 *  Parameter description table (drives the UI and preset files)
 * ------------------------------------------------------------------ */
typedef enum { PT_FLOAT, PT_INT, PT_ENUM, PT_BOOL } PType;

enum {
    PG_OSC, PG_FILTER, PG_ENV, PG_LFO, PG_FX, PG_SEQ, PG_MASTER, PG_COUNT
};

typedef struct {
    const char  *key;      /* short id used in preset files */
    const char  *name;     /* label shown in the UI          */
    PType        type;
    size_t       off;      /* offsetof() into Patch          */
    float        min, max;
    float        step;     /* coarse increment               */
    bool         logscale;
    const char  *unit;
    const char **labels;   /* for PT_ENUM                    */
    int          page;
} Param;

extern const Param  g_params[];
extern const int    g_nparams;
extern const char  *g_pagenames[PG_COUNT];

float       param_get(const Patch *p, const Param *pr);
void        param_set(Patch *p, const Param *pr, float v);
void        param_nudge(Patch *p, const Param *pr, int dir, bool fine);
void        param_format(const Patch *p, const Param *pr, char *out, size_t n);
float       param_norm(const Patch *p, const Param *pr); /* 0..1 for meters */

void        patch_init(Patch *p);
void        patch_randomize(Patch *p, uint32_t *rng);
int         patch_preset_count(void);
void        patch_load_preset(Patch *p, int idx);
int         patch_save_file(const Patch *p, const char *path);
int         patch_load_file(Patch *p, const char *path);

/* ------------------------------------------------------------------ *
 *  DSP building blocks
 * ------------------------------------------------------------------ */
enum { W_SINE, W_TRI, W_SAW, W_SQUARE, W_PULSE, W_NOISE, W_COUNT };
enum { F_LP24, F_LP12, F_BP12, F_BP24, F_HP24, F_COUNT };
enum { LW_SINE, LW_TRI, LW_SAW, LW_RAMP, LW_SQUARE, LW_SH, LW_COUNT };
enum { A_UP, A_DOWN, A_UPDOWN, A_DOWNUP, A_RANDOM, A_ASPLAYED, A_COUNT };

typedef enum { ENV_IDLE, ENV_ATK, ENV_DEC, ENV_SUS, ENV_REL } EnvStage;

typedef struct {
    EnvStage stage;
    float    level, ca, cd, cr, sustain;
} Env;

void  env_setup(Env *e, float a, float d, float s, float r, float sr);
void  env_gate(Env *e, bool on);
float env_run(Env *e);
static inline bool env_busy(const Env *e) { return e->stage != ENV_IDLE; }

typedef struct { float s[4]; } Ladder;
float ladder_run(Ladder *f, float x, float g, float k, float drive, int mode);

typedef struct {
    float *buf;
    int    size, wr;
} DLine;

int   dline_init(DLine *d, int size);
void  dline_free(DLine *d);
void  dline_clear(DLine *d);
float dline_read(const DLine *d, float delay);

typedef struct {
    DLine l, r;
    float lp_l, lp_r;
    float mod_ph;
} Delay;

typedef struct {
    DLine comb[2][8];
    float combz[2][8];
    DLine ap[2][4];
} Reverb;

typedef struct {
    DLine l, r;
    float ph;
} Chorus;

/* ------------------------------------------------------------------ *
 *  Voice
 * ------------------------------------------------------------------ */
typedef struct {
    bool     active, held;
    int      note;
    float    vel;
    double   ph1, ph2, phsub;
    float    freq, glidef;
    float    pan;
    Env      amp, fenv;
    Ladder   filt;
    uint32_t rng;
    float    shold;
    uint64_t started;
} Voice;

/* ------------------------------------------------------------------ *
 *  Sequencer
 * ------------------------------------------------------------------ */
typedef struct {
    int   note;   /* -1 = rest */
    float vel;
    bool  tie;
} Step;

/* ------------------------------------------------------------------ *
 *  Note events (UI thread -> audio thread, lock free)
 * ------------------------------------------------------------------ */
enum { EV_ON, EV_OFF, EV_ALLOFF, EV_PANIC };

typedef struct {
    uint8_t type, note;
    float   vel;
} Evt;

/* ------------------------------------------------------------------ *
 *  Engine
 * ------------------------------------------------------------------ */
typedef struct {
    Patch    patch;
    float    sr;

    Voice    voices[MAX_VOICES];
    uint64_t voicetick;

    /* held-note bookkeeping for the arpeggiator */
    bool     held[128];
    int      heldorder[128], nheld;

    /* global lfo */
    double   lfoph;
    float    lfoval, lfosh;
    uint32_t lrng;

    /* clocks (sample counters, audio thread owned) */
    double   arpclk, seqclk;
    int      step;          /* sequencer step 0..len-1   */
    int      arpidx, arpdir, arpoctcur;
    int      arpnotes[128], narp;

    /* scheduled note-offs for arp / sequencer driven notes */
    struct { int note; double t; bool on; } pending[32];

    bool     playing;       /* sequencer transport       */
    bool     recording;     /* step-record armed         */
    int      recstep;
    Step     seq[NSTEPS];

    /* effects */
    Chorus   chorus;
    Delay    delay;
    Reverb   reverb;

    /* event queue */
    Evt      q[EVQ_SIZE];
    _Atomic unsigned qhead, qtail;

    /* scope + metering */
    float    scope[SCOPE_LEN];
    _Atomic unsigned scopew;
    _Atomic int      nactive;
    _Atomic float    peakl, peakr;
    _Atomic float    cpu;
    _Atomic int      uistep;

    /* wav capture ring */
    float    *rec;
    _Atomic unsigned recw, recr;
    _Atomic bool      capturing;
} Engine;

void engine_init(Engine *e, float sr);
void engine_free(Engine *e);
void engine_render(Engine *e, float *out, int frames);
void engine_push(Engine *e, int type, int note, float vel);
void engine_reset_fx(Engine *e);

/* ------------------------------------------------------------------ *
 *  Audio backend + wav writer
 * ------------------------------------------------------------------ */
int  audio_start(Engine *e, const char *device, unsigned rate, unsigned period);
void audio_stop(void);
const char *audio_info(void);

typedef struct WavWriter WavWriter;
WavWriter *wav_open(const char *path, int sr);
void       wav_write(WavWriter *w, const float *interleaved, int frames);
long       wav_frames(const WavWriter *w);
void       wav_close(WavWriter *w);

int  engine_render_file(Engine *e, const char *path, float seconds);

/* ------------------------------------------------------------------ *
 *  UI
 * ------------------------------------------------------------------ */
int  ui_run(Engine *e);
void ui_set_keyhold(int ms);

/* small helpers */
float       exp2_fast(float x);           /* 2^x, ~1e-5 relative error */
const char *note_name(int n, char *buf, size_t sz);

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float note_hz(float n) {
    return 440.0f * exp2_fast((n - 69.0f) / 12.0f);
}

#endif /* SYNTH_H */
