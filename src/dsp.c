/* dsp.c - envelopes, zero-delay-feedback ladder filter, delay lines */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "synth.h"

/* 2^x for the audio path: exact enough for pitch, much cheaper than powf */
float exp2_fast(float x)
{
    union { float f; int32_t i; } u;
    float xi = floorf(x);
    float xf = x - xi;
    /* degree-4 minimax polynomial for 2^xf on [0,1) */
    float p = 1.0f + xf * (0.6931530f + xf * (0.2401536f +
              xf * (0.0558282f + xf * 0.0089893f)));
    int e = (int)xi;
    if (e < -126) return 0.0f;
    if (e >  127) return 3.4e38f;
    u.i = (e + 127) << 23;
    return p * u.f;
}

const char *note_name(int n, char *buf, size_t sz)
{
    static const char *nm[12] = { "C","C#","D","D#","E","F","F#","G",
                                  "G#","A","A#","B" };
    if (n < 0 || n > 127) { snprintf(buf, sz, "--"); return buf; }
    snprintf(buf, sz, "%s%d", nm[n % 12], n / 12 - 1);
    return buf;
}

/* ---------------------------------------------------------------- *
 *  ADSR - analog style, exponential segments with target overshoot
 * ---------------------------------------------------------------- */
static float seg_coef(float seconds, float sr)
{
    if (seconds < 0.0005f) seconds = 0.0005f;
    return 1.0f - expf(-1.0f / (seconds * sr));
}

void env_setup(Env *e, float a, float d, float s, float r, float sr)
{
    e->ca = seg_coef(a, sr);
    e->cd = seg_coef(d, sr);
    e->cr = seg_coef(r, sr);
    e->sustain = clampf(s, 0.0f, 1.0f);
}

void env_gate(Env *e, bool on)
{
    if (on) e->stage = ENV_ATK;
    else if (e->stage != ENV_IDLE) e->stage = ENV_REL;
}

float env_run(Env *e)
{
    switch (e->stage) {
    case ENV_IDLE:
        return 0.0f;
    case ENV_ATK:
        /* aim past 1.0 so the attack keeps a snappy, near-linear slope */
        e->level += (1.28f - e->level) * e->ca;
        if (e->level >= 1.0f) { e->level = 1.0f; e->stage = ENV_DEC; }
        break;
    case ENV_DEC:
        e->level += (e->sustain - 0.02f - e->level) * e->cd;
        if (e->level <= e->sustain + 0.0005f) {
            e->level = e->sustain;
            e->stage = e->sustain > 0.0001f ? ENV_SUS : ENV_IDLE;
        }
        break;
    case ENV_SUS:
        e->level = e->sustain;
        break;
    case ENV_REL:
        e->level += (-0.02f - e->level) * e->cr;
        if (e->level <= 0.0002f) { e->level = 0.0f; e->stage = ENV_IDLE; }
        break;
    }
    return e->level;
}

/* ---------------------------------------------------------------- *
 *  Zero-delay-feedback transistor ladder (Zavalishin TPT topology)
 *  with Oberheim-style stage mixing for the extra filter modes.
 * ---------------------------------------------------------------- */
static inline float tanh_fast(float x)
{
    /* Pade 3/2 approximation, saturating outside +-3 */
    if (x < -3.0f) return -1.0f;
    if (x >  3.0f) return  1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

float ladder_run(Ladder *f, float x, float g, float k, float drive, int mode)
{
    const float G  = g / (1.0f + g);
    const float G2 = G * G, G3 = G2 * G, G4 = G3 * G;

    /* resolve the instantaneous feedback path in one shot */
    float s0 = f->s[0] / (1.0f + g), s1 = f->s[1] / (1.0f + g);
    float s2 = f->s[2] / (1.0f + g), s3 = f->s[3] / (1.0f + g);
    float S  = G3 * s0 + G2 * s1 + G * s2 + s3;

    float u = (x * drive - k * S) / (1.0f + k * G4);
    u = tanh_fast(u);

    float y[4];
    float in = u;
    for (int i = 0; i < 4; i++) {
        float v = (in - f->s[i]) * G;
        y[i] = v + f->s[i];
        f->s[i] = y[i] + v;
        in = y[i];
    }

    switch (mode) {
    case F_LP24: return y[3];
    case F_LP12: return y[1];
    case F_BP12: return 2.0f * (y[1] - y[3]);
    case F_BP24: return 4.0f * (y[1] - 2.0f * y[2] + y[3]);
    case F_HP24: return u - 4.0f * y[0] + 6.0f * y[1] - 4.0f * y[2] + y[3];
    }
    return y[3];
}

/* ---------------------------------------------------------------- *
 *  Fractional delay line
 * ---------------------------------------------------------------- */
int dline_init(DLine *d, int size)
{
    d->buf = calloc((size_t)size, sizeof(float));
    if (!d->buf) return -1;
    d->size = size;
    d->wr = 0;
    return 0;
}

void dline_free(DLine *d)
{
    free(d->buf);
    d->buf = NULL;
    d->size = 0;
}

void dline_clear(DLine *d)
{
    if (d->buf) memset(d->buf, 0, (size_t)d->size * sizeof(float));
}

float dline_read(const DLine *d, float delay)
{
    if (!isfinite(delay)) return 0.0f;
    if (delay < 1.0f) delay = 1.0f;
    if (delay > (float)(d->size - 2)) delay = (float)(d->size - 2);

    /* Wrap in integers.  Adding size to a very small negative read position
     * can round straight up to size itself in float, which used to walk one
     * element off the end of the buffer. */
    float rp = (float)d->wr - delay;
    float fl = floorf(rp);
    float fr = rp - fl;
    int   i0 = (int)fl % d->size;
    if (i0 < 0) i0 += d->size;
    int   i1 = i0 + 1 == d->size ? 0 : i0 + 1;
    return d->buf[i0] + (d->buf[i1] - d->buf[i0]) * fr;
}
