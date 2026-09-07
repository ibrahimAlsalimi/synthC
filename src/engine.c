/* engine.c - oscillators, voice allocation, arp/sequencer clock, effects */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "synth.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================================================================ *
 *  Oscillators
 * ================================================================ */
static inline uint32_t xrng(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}
static inline float white(uint32_t *s)
{
    return (float)((int32_t)(xrng(s) >> 9) - 4194304) * (1.0f / 4194304.0f);
}

/* PolyBLEP residual: removes the worst of the aliasing at a step edge */
static inline float blep(float t, float dt)
{
    if (t < dt)            { t /= dt;             return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt)     { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static inline float wrap1(float x) { return x - floorf(x); }

static float osc_run(int wave, double *ph, float dt, float pw, uint32_t *rng)
{
    float t = (float)*ph;
    float y;

    switch (wave) {
    case W_SINE:
        y = sinf(2.0f * (float)M_PI * t);
        break;
    case W_TRI:
        y = 4.0f * fabsf(t - 0.5f) - 1.0f;   /* harmonics fall as 1/n^2 */
        break;
    case W_SAW:
        y = 2.0f * t - 1.0f - blep(t, dt);
        break;
    case W_SQUARE:
        y = t < 0.5f ? 1.0f : -1.0f;
        y += blep(t, dt) - blep(wrap1(t + 0.5f), dt);
        break;
    case W_PULSE:
        y = t < pw ? 1.0f : -1.0f;
        y += blep(t, dt) - blep(wrap1(t + 1.0f - pw), dt);
        y *= 1.0f + 0.6f * (0.5f - fabsf(pw - 0.5f)); /* level compensation */
        break;
    case W_NOISE:
    default:
        return white(rng);
    }

    *ph += dt;
    if (*ph >= 1.0) *ph -= 1.0;
    return y;
}

/* ================================================================ *
 *  Effects
 * ================================================================ */
static const int comb_tune[8]  = { 1116,1188,1277,1356,1422,1491,1557,1617 };
static const int ap_tune[4]    = { 556, 441, 341, 225 };
#define STEREO_SPREAD 23

static inline float ring_tap(DLine *d)      { return d->buf[d->wr]; }
static inline void  ring_push(DLine *d, float v)
{
    d->buf[d->wr] = v;
    if (++d->wr >= d->size) d->wr = 0;
}

static void reverb_init(Reverb *r, float sr)
{
    float k = sr / 44100.0f;
    for (int c = 0; c < 2; c++) {
        int sp = c * STEREO_SPREAD;
        for (int i = 0; i < 8; i++)
            dline_init(&r->comb[c][i], (int)((comb_tune[i] + sp) * k) + 1);
        for (int i = 0; i < 4; i++)
            dline_init(&r->ap[c][i], (int)((ap_tune[i] + sp) * k) + 1);
        memset(r->combz[c], 0, sizeof r->combz[c]);
    }
}

static void reverb_free(Reverb *r)
{
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 8; i++) dline_free(&r->comb[c][i]);
        for (int i = 0; i < 4; i++) dline_free(&r->ap[c][i]);
    }
}

static void reverb_clear(Reverb *r)
{
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 8; i++) dline_clear(&r->comb[c][i]);
        for (int i = 0; i < 4; i++) dline_clear(&r->ap[c][i]);
        memset(r->combz[c], 0, sizeof r->combz[c]);
    }
}

static void reverb_run(Reverb *r, float in, float fb, float damp,
                       float *outl, float *outr)
{
    float x = in * 0.015f;
    float out[2];

    for (int c = 0; c < 2; c++) {
        float acc = 0.0f;
        for (int i = 0; i < 8; i++) {
            float y = ring_tap(&r->comb[c][i]);
            r->combz[c][i] = y * (1.0f - damp) + r->combz[c][i] * damp;
            ring_push(&r->comb[c][i], x + r->combz[c][i] * fb);
            acc += y;
        }
        for (int i = 0; i < 4; i++) {
            float bufout = ring_tap(&r->ap[c][i]);
            float y = bufout - acc;
            ring_push(&r->ap[c][i], acc + bufout * 0.5f);
            acc = y;
        }
        out[c] = acc;
    }
    *outl = out[0];
    *outr = out[1];
}

/* ================================================================ *
 *  Engine
 * ================================================================ */
void engine_init(Engine *e, float sr)
{
    memset(e, 0, sizeof(*e));
    e->sr = sr;
    patch_load_preset(&e->patch, 0);

    for (int i = 0; i < MAX_VOICES; i++) e->voices[i].rng = 0x1234567u + i * 7919u;
    e->lrng = 0xB16B00B5u;
    e->arpdir = 1;

    dline_init(&e->chorus.l, (int)(sr * 0.05f));
    dline_init(&e->chorus.r, (int)(sr * 0.05f));
    dline_init(&e->delay.l,  (int)(sr * 2.0f));
    dline_init(&e->delay.r,  (int)(sr * 2.0f));
    reverb_init(&e->reverb, sr);

    e->rec = calloc(RECRING, sizeof(float));

    for (int i = 0; i < NSTEPS; i++) e->seq[i].note = -1;
    atomic_store(&e->qhead, 0);
    atomic_store(&e->qtail, 0);
}

void engine_free(Engine *e)
{
    dline_free(&e->chorus.l); dline_free(&e->chorus.r);
    dline_free(&e->delay.l);  dline_free(&e->delay.r);
    reverb_free(&e->reverb);
    free(e->rec);
}

void engine_reset_fx(Engine *e)
{
    dline_clear(&e->chorus.l); dline_clear(&e->chorus.r);
    dline_clear(&e->delay.l);  dline_clear(&e->delay.r);
    reverb_clear(&e->reverb);
    e->delay.lp_l = e->delay.lp_r = 0.0f;
}

void engine_push(Engine *e, int type, int note, float vel)
{
    unsigned h = atomic_load_explicit(&e->qhead, memory_order_relaxed);
    unsigned n = (h + 1) % EVQ_SIZE;
    if (n == atomic_load_explicit(&e->qtail, memory_order_acquire)) return; /* full */
    e->q[h].type = (uint8_t)type;
    e->q[h].note = (uint8_t)(note < 0 ? 0 : note > 127 ? 127 : note);
    e->q[h].vel  = vel;
    atomic_store_explicit(&e->qhead, n, memory_order_release);
}

/* ---------------- voice allocation ------------------------------ */
static void voice_start(Engine *e, int note, float vel)
{
    Patch *p = &e->patch;
    int limit = p->maxvoices < 1 ? 1 : p->maxvoices;
    Voice *v = NULL;

    /* retrigger the same note if it is already sounding */
    for (int i = 0; i < limit; i++)
        if (e->voices[i].active && e->voices[i].note == note) { v = &e->voices[i]; break; }

    if (!v)  /* free slot */
        for (int i = 0; i < limit; i++)
            if (!e->voices[i].active) { v = &e->voices[i]; break; }

    if (!v) { /* steal a released voice if there is one, else the oldest */
        uint64_t best = UINT64_MAX;
        int bestheld = 2;
        for (int i = 0; i < limit; i++) {
            int held = e->voices[i].held ? 1 : 0;
            uint64_t age = e->voices[i].started;
            if (held < bestheld || (held == bestheld && age < best)) {
                bestheld = held; best = age; v = &e->voices[i];
            }
        }
    }
    if (!v) return;

    float prev = v->glidef;
    bool  reuse = v->active && p->glide > 0.5f;

    v->note = note;
    v->vel  = vel;
    v->freq = note_hz((float)(note + p->transpose));
    v->glidef = reuse ? prev : v->freq;
    if (!v->active) {
        v->ph1 = 0.0; v->ph2 = 0.11; v->phsub = 0.0;
    }
    v->active = true;
    v->held = true;
    v->started = ++e->voicetick;

    int idx = (int)(v - e->voices);
    float sp = p->spread;
    v->pan = 0.5f + sp * 0.5f * ((idx % 2) ? 1.0f : -1.0f)
                  * (0.4f + 0.6f * (float)((idx / 2) % 4) / 3.0f);

    env_setup(&v->amp,  p->aA, p->aD, p->aS, p->aR, e->sr);
    env_setup(&v->fenv, p->fA, p->fD, p->fS, p->fR, e->sr);
    env_gate(&v->amp, true);
    env_gate(&v->fenv, true);
}

static void voice_stop(Engine *e, int note)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &e->voices[i];
        if (v->active && v->held && v->note == note) {
            v->held = false;
            env_gate(&v->amp, false);
            env_gate(&v->fenv, false);
        }
    }
}

static void all_off(Engine *e, bool panic)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &e->voices[i];
        if (panic) { memset(v, 0, sizeof *v); v->rng = 0x1234567u + i * 7919u; }
        else if (v->active) { v->held = false; env_gate(&v->amp, false); env_gate(&v->fenv, false); }
    }
    for (unsigned i = 0; i < sizeof e->pending / sizeof e->pending[0]; i++)
        e->pending[i].on = false;
    memset(e->held, 0, sizeof e->held);
    e->nheld = 0;
}

/* ---------------- held note list (for the arpeggiator) ---------- */
static void held_add(Engine *e, int note)
{
    if (e->held[note]) return;
    e->held[note] = true;
    if (e->nheld < 128) e->heldorder[e->nheld++] = note;
}

static void held_remove(Engine *e, int note)
{
    if (!e->held[note]) return;
    e->held[note] = false;
    for (int i = 0; i < e->nheld; i++)
        if (e->heldorder[i] == note) {
            memmove(&e->heldorder[i], &e->heldorder[i + 1],
                    (size_t)(e->nheld - i - 1) * sizeof(int));
            e->nheld--;
            break;
        }
}

/* ---------------- scheduled note offs --------------------------- */
static void sched_off(Engine *e, int note, double samples)
{
    for (unsigned i = 0; i < sizeof e->pending / sizeof e->pending[0]; i++)
        if (!e->pending[i].on) {
            e->pending[i].on = true;
            e->pending[i].note = note;
            e->pending[i].t = samples;
            return;
        }
    voice_stop(e, note);   /* table full: cut it now rather than hang */
}

static void pending_tick(Engine *e, double dt)
{
    for (unsigned i = 0; i < sizeof e->pending / sizeof e->pending[0]; i++)
        if (e->pending[i].on) {
            e->pending[i].t -= dt;
            if (e->pending[i].t <= 0.0) {
                e->pending[i].on = false;
                voice_stop(e, e->pending[i].note);
            }
        }
}

/* ---------------- clock helpers --------------------------------- */
static float div_beats(int d)
{
    static const float b[8] = { 4.0f, 2.0f, 1.0f, 0.5f, 1.0f/3.0f,
                                0.25f, 1.0f/6.0f, 0.125f };
    if (d < 0) d = 0;
    if (d > 7) d = 7;
    return b[d];
}

static void arp_build(Engine *e)
{
    Patch *p = &e->patch;
    int base[128], n = e->nheld;
    if (n <= 0) { e->narp = 0; return; }

    memcpy(base, e->heldorder, (size_t)n * sizeof(int));
    if (p->arpmode != A_ASPLAYED && p->arpmode != A_RANDOM) {
        for (int i = 1; i < n; i++) {           /* insertion sort, low->high */
            int k = base[i], j = i - 1;
            while (j >= 0 && base[j] > k) { base[j + 1] = base[j]; j--; }
            base[j + 1] = k;
        }
    }

    int oct = p->arpoct < 1 ? 1 : p->arpoct;
    e->narp = 0;
    for (int o = 0; o < oct; o++)
        for (int i = 0; i < n && e->narp < 128; i++) {
            int note = base[i] + 12 * o;
            e->arpnotes[e->narp++] = note > 127 ? 127 : note;
        }
    if (p->arpmode == A_DOWN || p->arpmode == A_DOWNUP) {
        for (int i = 0; i < e->narp / 2; i++) {
            int t = e->arpnotes[i];
            e->arpnotes[i] = e->arpnotes[e->narp - 1 - i];
            e->arpnotes[e->narp - 1 - i] = t;
        }
    }
}

static void arp_step(Engine *e)
{
    Patch *p = &e->patch;
    arp_build(e);
    if (e->narp == 0) return;

    int idx;
    if (p->arpmode == A_RANDOM) {
        idx = (int)(xrng(&e->lrng) % (uint32_t)e->narp);
    } else if (p->arpmode == A_UPDOWN || p->arpmode == A_DOWNUP) {
        idx = e->arpidx;
        if (e->narp > 1) {
            e->arpidx += e->arpdir;
            if (e->arpidx >= e->narp) { e->arpidx = e->narp - 2; e->arpdir = -1; }
            else if (e->arpidx < 0)   { e->arpidx = 1;           e->arpdir =  1; }
        } else e->arpidx = 0;
        if (idx >= e->narp) idx = e->narp - 1;
        if (idx < 0) idx = 0;
    } else {
        idx = e->arpidx % e->narp;
        e->arpidx = (e->arpidx + 1) % e->narp;
    }

    float steps = div_beats(p->arpdiv) * 60.0f / p->bpm * e->sr;
    voice_start(e, e->arpnotes[idx], 0.9f);
    sched_off(e, e->arpnotes[idx], steps * clampf(p->arpgate, 0.05f, 0.99f));
}

static void seq_step(Engine *e)
{
    Patch *p = &e->patch;
    int len = p->seqlen < 1 ? 1 : p->seqlen;
    e->step = (e->step + 1) % len;
    atomic_store(&e->uistep, e->step);

    Step *s = &e->seq[e->step];
    if (s->note < 0) return;

    float steps = 0.25f * 60.0f / p->bpm * e->sr;
    voice_start(e, s->note, s->vel);
    sched_off(e, s->note, steps * (s->tie ? 0.98f : 0.55f));
}

/* ---------------- lfo ------------------------------------------- */
static float lfo_run(Engine *e)
{
    Patch *p = &e->patch;
    double inc = p->lrate / e->sr;
    float  t   = (float)e->lfoph;
    float  y;

    switch (p->lwave) {
    case LW_SINE:   y = sinf(2.0f * (float)M_PI * t); break;
    case LW_TRI:    y = 4.0f * fabsf(t - 0.5f) - 1.0f; break;
    case LW_SAW:    y = 1.0f - 2.0f * t; break;
    case LW_RAMP:   y = 2.0f * t - 1.0f; break;
    case LW_SQUARE: y = t < 0.5f ? 1.0f : -1.0f; break;
    case LW_SH:
    default:        y = e->lfosh; break;
    }
    e->lfoph += inc;
    if (e->lfoph >= 1.0) {
        e->lfoph -= 1.0;
        e->lfosh = white(&e->lrng);
    }
    return e->lfoval = y;
}

/* ================================================================ *
 *  Render
 * ================================================================ */
void engine_render(Engine *e, float *out, int frames)
{
    Patch *p = &e->patch;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* ---- drain incoming note events ---- */
    unsigned tail = atomic_load_explicit(&e->qtail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&e->qhead, memory_order_acquire);
    while (tail != head) {
        Evt ev = e->q[tail];
        tail = (tail + 1) % EVQ_SIZE;
        switch (ev.type) {
        case EV_ON:
            held_add(e, ev.note);
            if (!p->arpon) voice_start(e, ev.note, ev.vel);
            break;
        case EV_OFF:
            held_remove(e, ev.note);
            if (!p->arpon) voice_stop(e, ev.note);
            break;
        case EV_ALLOFF: all_off(e, false); break;
        case EV_PANIC:  all_off(e, true); engine_reset_fx(e); break;
        }
    }
    atomic_store_explicit(&e->qtail, tail, memory_order_release);

    /* ---- cached per-block values ---- */
    const float sr      = e->sr;
    const float nyq     = sr * 0.45f;
    const float glidec  = p->glide > 0.5f
                          ? 1.0f - expf(-1.0f / (p->glide * 0.001f * sr)) : 1.0f;
    const float reso    = p->res * 3.9f;
    const float sat     = 1.0f + p->sat * 8.0f;
    const float satcomp = 1.0f / (1.0f + p->sat * 2.0f);

    /* delay time */
    float dts = p->dlsync
        ? div_beats(p->dldiv) * 60.0f / p->bpm * sr
        : p->dltime * 0.001f * sr;
    dts = clampf(dts, 4.0f, (float)(e->delay.l.size - 4));
    const float ddamp = p->dldamp * 0.85f;

    /* reverb */
    const float rvfb   = p->rvsize * 0.28f + 0.7f;
    const float rvdamp = p->rvdamp * 0.4f;
    const float rvw    = p->rvwidth;

    /* chorus */
    const float chbase = 0.008f * sr;
    const float chdep  = p->chdepth * 0.005f * sr;

    float pkl = 0.0f, pkr = 0.0f;
    int   nact = 0;

    for (int n = 0; n < frames; n++) {
        /* ---- clocks ---- */
        double dt1 = 1.0;
        pending_tick(e, dt1);

        if (p->arpon && e->nheld > 0) {
            e->arpclk -= dt1;
            if (e->arpclk <= 0.0) {
                float per = div_beats(p->arpdiv) * 60.0f / p->bpm * sr;
                e->arpclk += per > 8.0f ? per : 8.0f;
                arp_step(e);
            }
        } else {
            e->arpclk = 0.0;
            e->arpidx = 0;
            e->arpdir = 1;
        }

        if (e->playing && p->seqon) {
            e->seqclk -= dt1;
            if (e->seqclk <= 0.0) {
                float per = 0.25f * 60.0f / p->bpm * sr;
                /* long-short pairs: even steps stretch, odd steps catch up */
                float sw  = ((e->step + 1) & 1) ? (1.0f - p->swing) : (1.0f + p->swing);
                e->seqclk += per * sw > 8.0f ? per * sw : 8.0f;
                seq_step(e);
            }
        }

        /* ---- lfo ---- */
        float lfo = lfo_run(e);

        /* ---- voices ---- */
        float dryl = 0.0f, dryr = 0.0f;
        int   live = 0;

        for (int i = 0; i < MAX_VOICES; i++) {
            Voice *v = &e->voices[i];
            if (!v->active) continue;

            float a = env_run(&v->amp);
            if (!env_busy(&v->amp)) { v->active = false; v->held = false; continue; }
            live++;

            float fe = env_run(&v->fenv);

            /* pitch */
            v->glidef += (v->freq - v->glidef) * glidec;
            float detcents = p->l2pitch * lfo;
            float f1 = v->glidef * exp2_fast(detcents / 1200.0f);
            float f2 = f1 * exp2_fast(p->detune / 1200.0f + (float)p->o2oct);

            float dt1o = clampf(f1 / sr, 0.0f, 0.45f);
            float dt2o = clampf(f2 / sr, 0.0f, 0.45f);
            float dtsu = clampf(f1 * 0.5f / sr, 0.0f, 0.45f);

            float pw = clampf(p->pw + p->l2pw * lfo * 0.45f, 0.03f, 0.97f);

            float s1 = osc_run(p->o1wave, &v->ph1, dt1o, pw, &v->rng);
            float s2 = osc_run(p->o2wave, &v->ph2, dt2o, pw, &v->rng);
            float sig = s1 * (1.0f - p->oscmix) + s2 * p->oscmix;

            if (p->sublevel > 0.0001f)
                sig += osc_run(W_SQUARE, &v->phsub, dtsu, 0.5f, &v->rng) * p->sublevel * 0.7f;
            if (p->noiselevel > 0.0001f)
                sig += white(&v->rng) * p->noiselevel * 0.5f;

            /* filter cutoff modulation */
            float kt  = p->keytrack * ((float)(v->note - 60) / 12.0f);
            float oct = p->fenvamt * 6.0f * fe + kt + p->l2cut * lfo;
            float fc  = clampf(p->cutoff * exp2_fast(oct), 20.0f, nyq);
            float g   = tanf((float)M_PI * fc / sr);

            float y = ladder_run(&v->filt, sig * 0.5f, g, reso, p->drive, p->fmode);

            float amp = a * v->vel * (1.0f - p->l2amp * 0.5f * (1.0f - lfo));
            y *= amp;

            dryl += y * (1.0f - v->pan);
            dryr += y * v->pan;
        }
        if (live > nact) nact = live;

        float l = dryl * 0.45f, r = dryr * 0.45f;

        /* ---- saturation ---- */
        if (p->sat > 0.0001f) {
            l = tanhf(l * sat) * satcomp;
            r = tanhf(r * sat) * satcomp;
        }

        /* ---- chorus ---- */
        if (p->chmix > 0.0001f) {
            e->chorus.ph += p->chrate / sr;
            if (e->chorus.ph >= 1.0) e->chorus.ph -= 1.0;
            float m1 = sinf(2.0f * (float)M_PI * (float)e->chorus.ph);
            float m2 = sinf(2.0f * (float)M_PI * ((float)e->chorus.ph + 0.25f));
            float cl = dline_read(&e->chorus.l, chbase + chdep * (1.0f + m1));
            float cr = dline_read(&e->chorus.r, chbase + chdep * (1.0f + m2));
            ring_push(&e->chorus.l, l);
            ring_push(&e->chorus.r, r);
            l = l * (1.0f - p->chmix * 0.5f) + cl * p->chmix;
            r = r * (1.0f - p->chmix * 0.5f) + cr * p->chmix;
        }

        /* ---- delay ---- */
        if (p->dlmix > 0.0001f || p->dlfb > 0.0001f) {
            float dl = dline_read(&e->delay.l, dts);
            float dr = dline_read(&e->delay.r, dts);
            e->delay.lp_l += (dl - e->delay.lp_l) * (1.0f - ddamp);
            e->delay.lp_r += (dr - e->delay.lp_r) * (1.0f - ddamp);
            float fl = e->delay.lp_l * p->dlfb;
            float fr = e->delay.lp_r * p->dlfb;
            if (p->dlping) { ring_push(&e->delay.l, r + fr); ring_push(&e->delay.r, l + fl); }
            else           { ring_push(&e->delay.l, l + fl); ring_push(&e->delay.r, r + fr); }
            l += dl * p->dlmix;
            r += dr * p->dlmix;
        }

        /* ---- reverb ---- */
        if (p->rvmix > 0.0001f) {
            float wl, wr;
            reverb_run(&e->reverb, l + r, rvfb, rvdamp, &wl, &wr);
            float w1 = 0.5f + rvw * 0.5f, w2 = (1.0f - rvw) * 0.5f;
            float ml = wl * w1 + wr * w2;
            float mr = wr * w1 + wl * w2;
            l = l * (1.0f - p->rvmix * 0.5f) + ml * p->rvmix * 1.1f;
            r = r * (1.0f - p->rvmix * 0.5f) + mr * p->rvmix * 1.1f;
        }

        /* ---- master ---- */
        l *= p->volume;
        r *= p->volume;
        l = tanhf(l * 1.1f);      /* gentle brickwall so nothing ever clips hard */
        r = tanhf(r * 1.1f);

        out[n * 2]     = l;
        out[n * 2 + 1] = r;

        float al = fabsf(l), ar = fabsf(r);
        if (al > pkl) pkl = al;
        if (ar > pkr) pkr = ar;

        unsigned sw = atomic_load_explicit(&e->scopew, memory_order_relaxed);
        e->scope[sw & (SCOPE_LEN - 1)] = (l + r) * 0.5f;
        atomic_store_explicit(&e->scopew, sw + 1, memory_order_relaxed);
    }

    /* ---- wav capture ---- */
    if (atomic_load_explicit(&e->capturing, memory_order_relaxed) && e->rec) {
        unsigned w = atomic_load_explicit(&e->recw, memory_order_relaxed);
        unsigned rd = atomic_load_explicit(&e->recr, memory_order_acquire);
        for (int i = 0; i < frames * 2; i += 2) {
            unsigned w1 = (w + 1) & (RECRING - 1);
            unsigned w2 = (w + 2) & (RECRING - 1);
            if (w1 == rd || w2 == rd) break;   /* consumer fell behind, drop */
            e->rec[w]  = out[i];
            e->rec[w1] = out[i + 1];
            w = w2;
        }
        atomic_store_explicit(&e->recw, w, memory_order_release);
    }

    atomic_store(&e->nactive, nact);
    atomic_store(&e->peakl, pkl);
    atomic_store(&e->peakr, pkr);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double used = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
    double avail = (double)frames / sr;
    float  cpu = (float)(used / avail * 100.0);
    float  prev = atomic_load(&e->cpu);
    atomic_store(&e->cpu, prev + (cpu - prev) * 0.1f);
}
