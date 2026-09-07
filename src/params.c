/* params.c - the parameter table, factory presets and preset file I/O */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "synth.h"

static const char *L_wave[]   = { "Sine","Tri","Saw","Square","Pulse","Noise",NULL };
static const char *L_fmode[]  = { "LP 24","LP 12","BP 12","BP 24","HP 24",NULL };
static const char *L_lwave[]  = { "Sine","Tri","Saw","Ramp","Square","S&H",NULL };
static const char *L_arp[]    = { "Up","Down","Up/Dn","Dn/Up","Random","As Played",NULL };
static const char *L_div[]    = { "1/1","1/2","1/4","1/8","1/8T","1/16","1/16T","1/32",NULL };
static const char *L_onoff[]  = { "off","on",NULL };
static const char *L_oct[]    = { "-2","-1","0","+1","+2",NULL };

const char *g_pagenames[PG_COUNT] = {
    "OSC", "FILTER", "ENV", "LFO", "FX", "SEQ", "MASTER"
};

#define P(k,n,t,f,mn,mx,st,lg,u,lb,pg) \
    { k, n, t, offsetof(Patch,f), mn, mx, st, lg, u, lb, pg }

const Param g_params[] = {
/* ---- oscillators ------------------------------------------------ */
P("o1wave","Osc 1 Wave",   PT_ENUM, o1wave,     0,W_COUNT-1, 1, false, NULL, L_wave,  PG_OSC),
P("o2wave","Osc 2 Wave",   PT_ENUM, o2wave,     0,W_COUNT-1, 1, false, NULL, L_wave,  PG_OSC),
P("mix",   "Osc 1<>2 Mix", PT_FLOAT,oscmix,     0,      1, 0.02f,false, "",   NULL,    PG_OSC),
P("det",   "Osc 2 Detune", PT_FLOAT,detune,   -50,     50,  1.0f,false, "ct", NULL,    PG_OSC),
P("o2oct", "Osc 2 Octave", PT_ENUM, o2oct,     -2,      2,  1,   false, NULL, L_oct,   PG_OSC),
P("pw",    "Pulse Width",  PT_FLOAT,pw,     0.02f,  0.98f, 0.01f,false, "",   NULL,    PG_OSC),
P("sub",   "Sub Osc",      PT_FLOAT,sublevel,   0,      1, 0.02f,false, "",   NULL,    PG_OSC),
P("noise", "Noise",        PT_FLOAT,noiselevel, 0,      1, 0.02f,false, "",   NULL,    PG_OSC),
P("glide", "Glide",        PT_FLOAT,glide,      0,   2000, 10.0f,false, "ms", NULL,    PG_OSC),
P("sprd",  "Stereo Spread",PT_FLOAT,spread,     0,      1, 0.02f,false, "",   NULL,    PG_OSC),
P("poly",  "Polyphony",    PT_INT,  maxvoices,  1,MAX_VOICES,1, false, "",   NULL,    PG_OSC),

/* ---- filter ----------------------------------------------------- */
P("cut",   "Cutoff",       PT_FLOAT,cutoff,   20,  18000, 0.05f,true,  "Hz", NULL,    PG_FILTER),
P("res",   "Resonance",    PT_FLOAT,res,       0,      1, 0.02f,false, "",   NULL,    PG_FILTER),
P("fmode", "Mode",         PT_ENUM, fmode,     0,F_COUNT-1, 1, false, NULL, L_fmode, PG_FILTER),
P("fenv",  "Env Amount",   PT_FLOAT,fenvamt,  -1,      1, 0.02f,false, "",   NULL,    PG_FILTER),
P("ktrk",  "Key Track",    PT_FLOAT,keytrack,  0,      1, 0.02f,false, "",   NULL,    PG_FILTER),
P("drv",   "Drive",        PT_FLOAT,drive,     1,      8, 0.1f, false, "x",  NULL,    PG_FILTER),

/* ---- envelopes -------------------------------------------------- */
P("aA","Amp Attack",   PT_FLOAT,aA,0.001f,10,0.06f,true, "s", NULL, PG_ENV),
P("aD","Amp Decay",    PT_FLOAT,aD,0.001f,10,0.06f,true, "s", NULL, PG_ENV),
P("aS","Amp Sustain",  PT_FLOAT,aS,0,      1,0.02f,false,"",  NULL, PG_ENV),
P("aR","Amp Release",  PT_FLOAT,aR,0.002f,10,0.06f,true, "s", NULL, PG_ENV),
P("fA","Filt Attack",  PT_FLOAT,fA,0.001f,10,0.06f,true, "s", NULL, PG_ENV),
P("fD","Filt Decay",   PT_FLOAT,fD,0.001f,10,0.06f,true, "s", NULL, PG_ENV),
P("fS","Filt Sustain", PT_FLOAT,fS,0,      1,0.02f,false,"",  NULL, PG_ENV),
P("fR","Filt Release", PT_FLOAT,fR,0.002f,10,0.06f,true, "s", NULL, PG_ENV),

/* ---- lfo -------------------------------------------------------- */
P("lw",   "LFO Wave",    PT_ENUM, lwave,  0,LW_COUNT-1,1,false,NULL,L_lwave,PG_LFO),
P("lr",   "LFO Rate",    PT_FLOAT,lrate,0.02f,   30, 0.05f,true, "Hz",NULL, PG_LFO),
P("l2p",  "-> Pitch",    PT_FLOAT,l2pitch, 0,   100, 1.0f, false,"ct",NULL, PG_LFO),
P("l2c",  "-> Cutoff",   PT_FLOAT,l2cut,  -4,     4, 0.05f,false,"oct",NULL,PG_LFO),
P("l2w",  "-> Pulse W",  PT_FLOAT,l2pw,    0,     1, 0.02f,false,"",  NULL, PG_LFO),
P("l2a",  "-> Amp",      PT_FLOAT,l2amp,   0,     1, 0.02f,false,"",  NULL, PG_LFO),

/* ---- effects ---------------------------------------------------- */
P("sat",  "Saturation",  PT_FLOAT,sat,     0,     1, 0.02f,false,"",  NULL, PG_FX),
P("chd",  "Chorus Depth",PT_FLOAT,chdepth, 0,     1, 0.02f,false,"",  NULL, PG_FX),
P("chr",  "Chorus Rate", PT_FLOAT,chrate,0.05f,  8, 0.05f,true, "Hz",NULL, PG_FX),
P("chm",  "Chorus Mix",  PT_FLOAT,chmix,   0,     1, 0.02f,false,"",  NULL, PG_FX),
P("dsy",  "Delay Sync",  PT_BOOL, dlsync,  0,     1, 1,    false,NULL,L_onoff,PG_FX),
P("ddv",  "Delay Div",   PT_ENUM, dldiv,   0,     7, 1,    false,NULL,L_div, PG_FX),
P("dt",   "Delay Time",  PT_FLOAT,dltime, 10,  1500, 0.05f,true, "ms",NULL, PG_FX),
P("dfb",  "Delay Fdbk",  PT_FLOAT,dlfb,    0, 0.95f,0.02f,false,"",  NULL, PG_FX),
P("ddp",  "Delay Damp",  PT_FLOAT,dldamp,  0,     1, 0.02f,false,"",  NULL, PG_FX),
P("dpp",  "Delay Ping",  PT_BOOL, dlping,  0,     1, 1,    false,NULL,L_onoff,PG_FX),
P("dmx",  "Delay Mix",   PT_FLOAT,dlmix,   0,     1, 0.02f,false,"",  NULL, PG_FX),
P("rsz",  "Reverb Size", PT_FLOAT,rvsize,  0,     1, 0.02f,false,"",  NULL, PG_FX),
P("rdp",  "Reverb Damp", PT_FLOAT,rvdamp,  0,     1, 0.02f,false,"",  NULL, PG_FX),
P("rwd",  "Reverb Width",PT_FLOAT,rvwidth, 0,     1, 0.02f,false,"",  NULL, PG_FX),
P("rmx",  "Reverb Mix",  PT_FLOAT,rvmix,   0,     1, 0.02f,false,"",  NULL, PG_FX),

/* ---- arp / sequencer -------------------------------------------- */
P("bpm",  "Tempo",       PT_FLOAT,bpm,    30,   300, 1.0f, false,"bpm",NULL,PG_SEQ),
P("arp",  "Arp",         PT_BOOL, arpon,   0,     1, 1,    false,NULL,L_onoff,PG_SEQ),
P("amd",  "Arp Mode",    PT_ENUM, arpmode, 0,A_COUNT-1,1,  false,NULL,L_arp, PG_SEQ),
P("adv",  "Arp Rate",    PT_ENUM, arpdiv,  0,     7, 1,    false,NULL,L_div, PG_SEQ),
P("aoc",  "Arp Octaves", PT_INT,  arpoct,  1,     4, 1,    false,"",  NULL,  PG_SEQ),
P("agt",  "Gate Length", PT_FLOAT,arpgate,0.05f,1.0f,0.02f,false,"",  NULL,  PG_SEQ),
P("seq",  "Sequencer",   PT_BOOL, seqon,   0,     1, 1,    false,NULL,L_onoff,PG_SEQ),
P("slen", "Seq Length",  PT_INT,  seqlen,  1,NSTEPS,1,     false,"",  NULL,  PG_SEQ),
P("swg",  "Swing",       PT_FLOAT,swing,   0, 0.75f,0.02f, false,"",  NULL,  PG_SEQ),

/* ---- master ----------------------------------------------------- */
P("vol",  "Volume",      PT_FLOAT,volume,  0,  1.5f,0.02f, false,"",  NULL,  PG_MASTER),
P("trn",  "Transpose",   PT_INT,  transpose,-24, 24, 1,    false,"st",NULL,  PG_MASTER),
};

const int g_nparams = (int)(sizeof(g_params) / sizeof(g_params[0]));

/* ---------------------------------------------------------------- */

static float *fptr(Patch *p, const Param *pr) { return (float *)((char *)p + pr->off); }
static int   *iptr(Patch *p, const Param *pr) { return (int   *)((char *)p + pr->off); }

float param_get(const Patch *p, const Param *pr)
{
    Patch *m = (Patch *)p;
    return pr->type == PT_FLOAT ? *fptr(m, pr) : (float)*iptr(m, pr);
}

void param_set(Patch *p, const Param *pr, float v)
{
    v = clampf(v, pr->min, pr->max);
    if (pr->type == PT_FLOAT) *fptr(p, pr) = v;
    else                      *iptr(p, pr) = (int)lrintf(v);
}

void param_nudge(Patch *p, const Param *pr, int dir, bool fine)
{
    float v = param_get(p, pr);

    if (pr->type != PT_FLOAT) {
        param_set(p, pr, v + (float)dir);
        return;
    }
    if (pr->logscale) {
        float k = 1.0f + pr->step * (fine ? 0.2f : 1.0f);
        v = dir > 0 ? v * k : v / k;
    } else {
        v += (float)dir * pr->step * (fine ? 0.2f : 1.0f);
    }
    param_set(p, pr, v);
}

float param_norm(const Patch *p, const Param *pr)
{
    float v = param_get(p, pr);
    if (pr->logscale && pr->min > 0.0f)
        return (logf(v / pr->min)) / logf(pr->max / pr->min);
    return (v - pr->min) / (pr->max - pr->min);
}

void param_format(const Patch *p, const Param *pr, char *out, size_t n)
{
    float v = param_get(p, pr);

    if (pr->labels) {
        int i = (int)lrintf(v - pr->min);
        int cnt = 0;
        while (pr->labels[cnt]) cnt++;
        if (i < 0) i = 0;
        if (i >= cnt) i = cnt - 1;
        snprintf(out, n, "%s", pr->labels[i]);
        return;
    }
    if (pr->type == PT_INT) {
        const char *un = (pr->unit && *pr->unit) ? pr->unit : "";
        if (pr->min < 0) snprintf(out, n, "%+d %s", (int)v, un);
        else             snprintf(out, n, "%d %s", (int)v, un);
        return;
    }
    if (pr->unit && !strcmp(pr->unit, "s")) {
        if (v < 1.0f) snprintf(out, n, "%.0f ms", v * 1000.0f);
        else          snprintf(out, n, "%.2f s", v);
        return;
    }
    if (pr->unit && !strcmp(pr->unit, "Hz")) {
        if (v >= 1000.0f) snprintf(out, n, "%.2f kHz", v / 1000.0f);
        else if (v >= 10.0f) snprintf(out, n, "%.0f Hz", v);
        else snprintf(out, n, "%.2f Hz", v);
        return;
    }
    if (pr->unit && *pr->unit) snprintf(out, n, "%.0f %s", v, pr->unit);
    else                       snprintf(out, n, "%.2f", v);
}

/* ---------------------------------------------------------------- *
 *  Factory patch + presets
 * ---------------------------------------------------------------- */
void patch_init(Patch *p)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof p->name, "Init");

    p->o1wave = W_SAW;  p->o2wave = W_SAW;
    p->oscmix = 0.4f;   p->detune = 7.0f;   p->o2oct = 0;
    p->pw = 0.5f;       p->sublevel = 0.0f; p->noiselevel = 0.0f;
    p->glide = 0.0f;    p->spread = 0.25f;  p->maxvoices = 8;

    p->cutoff = 2400.0f; p->res = 0.25f;   p->fmode = F_LP24;
    p->fenvamt = 0.45f;  p->keytrack = 0.4f; p->drive = 1.4f;

    p->aA = 0.005f; p->aD = 0.35f; p->aS = 0.7f;  p->aR = 0.30f;
    p->fA = 0.004f; p->fD = 0.50f; p->fS = 0.25f; p->fR = 0.35f;

    p->lwave = LW_SINE; p->lrate = 5.0f;
    p->l2pitch = 0; p->l2cut = 0; p->l2pw = 0; p->l2amp = 0;

    p->sat = 0.12f;
    p->chdepth = 0.35f; p->chrate = 0.6f; p->chmix = 0.2f;
    p->dlsync = 1; p->dldiv = 5; p->dltime = 375.0f;
    p->dlfb = 0.35f; p->dldamp = 0.35f; p->dlping = 1; p->dlmix = 0.18f;
    p->rvsize = 0.55f; p->rvdamp = 0.4f; p->rvwidth = 0.9f; p->rvmix = 0.16f;

    p->arpon = 0; p->arpmode = A_UP; p->arpdiv = 5; p->arpoct = 1;
    p->arpgate = 0.55f;
    p->seqon = 0; p->seqlen = 16; p->swing = 0.0f;
    p->bpm = 120.0f;

    p->volume = 0.7f; p->transpose = 0;
}

typedef void (*PresetFn)(Patch *);

static void ps_classic(Patch *p) { (void)p; }

static void ps_fatbass(Patch *p)
{
    p->o1wave = W_SAW; p->o2wave = W_SQUARE; p->oscmix = 0.45f;
    p->detune = 12; p->o2oct = -1; p->sublevel = 0.55f;
    p->cutoff = 620; p->res = 0.32f; p->fenvamt = 0.55f; p->keytrack = 0.3f;
    p->drive = 3.0f; p->sat = 0.35f;
    p->aA = 0.002f; p->aD = 0.5f; p->aS = 0.55f; p->aR = 0.15f;
    p->fA = 0.002f; p->fD = 0.22f; p->fS = 0.12f; p->fR = 0.2f;
    p->maxvoices = 4; p->spread = 0.1f;
    p->chmix = 0.0f; p->dlmix = 0.08f; p->rvmix = 0.06f;
}

static void ps_pluck(Patch *p)
{
    p->o1wave = W_SQUARE; p->o2wave = W_SAW; p->oscmix = 0.3f;
    p->detune = 5; p->pw = 0.32f;
    p->cutoff = 3200; p->res = 0.4f; p->fenvamt = 0.7f;
    p->aA = 0.001f; p->aD = 0.32f; p->aS = 0.0f; p->aR = 0.22f;
    p->fA = 0.001f; p->fD = 0.16f; p->fS = 0.0f; p->fR = 0.2f;
    p->dlmix = 0.28f; p->dlfb = 0.42f; p->rvmix = 0.22f;
}

static void ps_strings(Patch *p)
{
    p->o1wave = W_SAW; p->o2wave = W_SAW; p->oscmix = 0.5f;
    p->detune = 16; p->spread = 0.8f; p->maxvoices = 12;
    p->cutoff = 1800; p->res = 0.12f; p->fenvamt = 0.25f; p->keytrack = 0.6f;
    p->aA = 0.45f; p->aD = 1.2f; p->aS = 0.8f; p->aR = 0.9f;
    p->fA = 0.6f;  p->fD = 1.5f; p->fS = 0.5f; p->fR = 1.0f;
    p->lrate = 4.6f; p->l2pitch = 6;
    p->chdepth = 0.6f; p->chmix = 0.45f;
    p->rvsize = 0.8f; p->rvmix = 0.4f; p->dlmix = 0.1f;
}

static void ps_acid(Patch *p)
{
    p->o1wave = W_SAW; p->o2wave = W_SAW; p->oscmix = 0.0f;
    p->maxvoices = 1; p->glide = 60; p->spread = 0;
    p->cutoff = 320; p->res = 0.82f; p->fenvamt = 0.75f; p->drive = 4.0f;
    p->keytrack = 0.2f; p->sat = 0.5f;
    p->aA = 0.001f; p->aD = 0.4f; p->aS = 0.4f; p->aR = 0.12f;
    p->fA = 0.001f; p->fD = 0.24f; p->fS = 0.05f; p->fR = 0.12f;
    p->arpon = 1; p->arpdiv = 5; p->arpmode = A_UP; p->arpoct = 2;
    p->dlmix = 0.22f; p->dlfb = 0.45f; p->rvmix = 0.1f;
    p->chmix = 0;
}

static void ps_bell(Patch *p)
{
    p->o1wave = W_SINE; p->o2wave = W_SINE; p->oscmix = 0.5f;
    p->detune = 0; p->o2oct = 2; p->noiselevel = 0.04f;
    p->cutoff = 6000; p->res = 0.2f; p->fenvamt = 0.5f; p->drive = 1.0f;
    p->aA = 0.001f; p->aD = 2.2f; p->aS = 0.0f; p->aR = 1.8f;
    p->fA = 0.001f; p->fD = 1.0f; p->fS = 0.0f; p->fR = 1.0f;
    p->rvsize = 0.85f; p->rvmix = 0.45f; p->dlmix = 0.3f; p->dlfb = 0.5f;
    p->chmix = 0.2f;
}

static void ps_drone(Patch *p)
{
    p->o1wave = W_TRI; p->o2wave = W_PULSE; p->oscmix = 0.4f;
    p->detune = 24; p->o2oct = -1; p->pw = 0.22f; p->sublevel = 0.3f;
    p->cutoff = 900; p->res = 0.45f; p->fenvamt = 0.1f;
    p->aA = 1.5f; p->aD = 2.0f; p->aS = 0.55f; p->aR = 1.8f;
    p->fA = 2.5f; p->fD = 3.0f; p->fS = 0.6f;  p->fR = 2.5f;
    p->lwave = LW_TRI; p->lrate = 0.13f; p->l2cut = 1.6f; p->l2pw = 0.4f;
    p->rvsize = 0.95f; p->rvmix = 0.35f; p->chmix = 0.3f; p->dlmix = 0.2f;
    p->maxvoices = 8; p->spread = 1.0f; p->sublevel = 0.2f;
}

static void ps_lead(Patch *p)
{
    p->o1wave = W_PULSE; p->o2wave = W_SAW; p->oscmix = 0.35f;
    p->detune = 9; p->pw = 0.35f; p->maxvoices = 1; p->glide = 40;
    p->cutoff = 3000; p->res = 0.3f; p->fenvamt = 0.35f; p->drive = 2.2f;
    p->aA = 0.01f; p->aD = 0.6f; p->aS = 0.85f; p->aR = 0.25f;
    p->fA = 0.02f; p->fD = 0.5f; p->fS = 0.4f;  p->fR = 0.3f;
    p->lrate = 5.6f; p->l2pitch = 14; p->l2pw = 0.25f;
    p->dlmix = 0.3f; p->dlfb = 0.4f; p->rvmix = 0.2f; p->sat = 0.3f;
}

static void ps_wobble(Patch *p)
{
    p->o1wave = W_SAW; p->o2wave = W_SQUARE; p->oscmix = 0.5f;
    p->detune = 20; p->o2oct = -1; p->sublevel = 0.4f;
    p->cutoff = 400; p->res = 0.7f; p->drive = 5.0f; p->sat = 0.6f;
    p->fenvamt = 0.2f;
    p->aA = 0.005f; p->aD = 1.0f; p->aS = 0.9f; p->aR = 0.2f;
    p->lwave = LW_TRI; p->lrate = 3.0f; p->l2cut = 2.6f;
    p->maxvoices = 4; p->dlmix = 0.15f; p->rvmix = 0.1f;
}

static const struct { const char *name; PresetFn fn; } g_presets[] = {
    { "Init Saw",    ps_classic },
    { "Fat Bass",    ps_fatbass },
    { "Pluck",       ps_pluck   },
    { "Strings",     ps_strings },
    { "Acid Line",   ps_acid    },
    { "Bell Keys",   ps_bell    },
    { "Drone Pad",   ps_drone   },
    { "Solo Lead",   ps_lead    },
    { "Wobble",      ps_wobble  },
};

int patch_preset_count(void)
{
    return (int)(sizeof(g_presets) / sizeof(g_presets[0]));
}

void patch_load_preset(Patch *p, int idx)
{
    int n = patch_preset_count();
    idx = ((idx % n) + n) % n;
    patch_init(p);
    g_presets[idx].fn(p);
    snprintf(p->name, sizeof p->name, "%s", g_presets[idx].name);
}

static uint32_t xr(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}
static float frand(uint32_t *s) { return (float)(xr(s) >> 8) / 16777216.0f; }
static float rrange(uint32_t *s, float a, float b) { return a + (b - a) * frand(s); }

void patch_randomize(Patch *p, uint32_t *rng)
{
    float vol = p->volume, bpm = p->bpm;
    int   trn = p->transpose;

    p->o1wave = (int)(frand(rng) * W_COUNT);
    p->o2wave = (int)(frand(rng) * W_COUNT);
    p->oscmix = rrange(rng, 0.1f, 0.9f);
    p->detune = rrange(rng, 0, 25);
    p->o2oct  = (int)rrange(rng, -1.4f, 1.4f);
    p->pw     = rrange(rng, 0.15f, 0.85f);
    p->sublevel   = frand(rng) < 0.5f ? rrange(rng, 0, 0.6f) : 0;
    p->noiselevel = frand(rng) < 0.3f ? rrange(rng, 0, 0.2f) : 0;

    p->cutoff  = rrange(rng, 200, 6000);
    p->res     = rrange(rng, 0, 0.75f);
    p->fmode   = frand(rng) < 0.75f ? F_LP24 : (int)(frand(rng) * F_COUNT);
    p->fenvamt = rrange(rng, -0.4f, 0.9f);
    p->drive   = rrange(rng, 1, 4);

    p->aA = rrange(rng, 0.001f, 0.8f);
    p->aD = rrange(rng, 0.05f, 2.0f);
    p->aS = rrange(rng, 0.0f, 0.9f);
    p->aR = rrange(rng, 0.05f, 2.0f);
    p->fA = rrange(rng, 0.001f, 1.0f);
    p->fD = rrange(rng, 0.05f, 2.0f);
    p->fS = rrange(rng, 0.0f, 0.8f);
    p->fR = rrange(rng, 0.05f, 2.0f);

    p->lwave = (int)(frand(rng) * LW_COUNT);
    p->lrate = rrange(rng, 0.1f, 9.0f);
    p->l2pitch = frand(rng) < 0.4f ? rrange(rng, 0, 25) : 0;
    p->l2cut   = frand(rng) < 0.5f ? rrange(rng, -2, 2) : 0;
    p->l2pw    = rrange(rng, 0, 0.5f);

    p->sat   = rrange(rng, 0, 0.5f);
    p->chmix = rrange(rng, 0, 0.5f);
    p->dlmix = rrange(rng, 0, 0.4f);
    p->dlfb  = rrange(rng, 0.1f, 0.6f);
    p->rvmix = rrange(rng, 0, 0.45f);
    p->rvsize= rrange(rng, 0.3f, 0.95f);

    p->volume = vol; p->bpm = bpm; p->transpose = trn;
    snprintf(p->name, sizeof p->name, "Random");
}

/* ---------------------------------------------------------------- *
 *  Preset files: plain "key value" text, easy to hand-edit
 * ---------------------------------------------------------------- */
int patch_save_file(const Patch *p, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# %s patch\nname %s\n", APP_NAME, p->name);
    for (int i = 0; i < g_nparams; i++) {
        const Param *pr = &g_params[i];
        fprintf(f, "%s %.6g\n", pr->key, param_get(p, pr));
    }
    fclose(f);
    return 0;
}

int patch_load_file(Patch *p, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64]; char rest[160];
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%63s %159[^\n]", key, rest) != 2) continue;
        if (!strcmp(key, "name")) {
            snprintf(p->name, sizeof p->name, "%.31s", rest);
            continue;
        }
        for (int i = 0; i < g_nparams; i++)
            if (!strcmp(key, g_params[i].key))
                param_set(p, &g_params[i], (float)atof(rest));
    }
    fclose(f);
    return 0;
}
