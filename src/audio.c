/* audio.c - ALSA playback thread, WAV writer and offline rendering */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>
#include "synth.h"

static snd_pcm_t      *g_pcm;
static pthread_t       g_thread;
static atomic_bool     g_run;
static Engine         *g_engine;
static unsigned        g_period;
static char            g_info[160] = "not started";

const char *audio_info(void) { return g_info; }

static void *audio_loop(void *arg)
{
    Engine *e = arg;
    float *buf = malloc(sizeof(float) * g_period * 2);
    if (!buf) return NULL;

    while (atomic_load(&g_run)) {
        engine_render(e, buf, (int)g_period);

        snd_pcm_sframes_t w = snd_pcm_writei(g_pcm, buf, g_period);
        if (w < 0) {
            w = snd_pcm_recover(g_pcm, (int)w, 1);
            if (w < 0) { snd_pcm_prepare(g_pcm); }
        }
    }
    free(buf);
    return NULL;
}

int audio_start(Engine *e, const char *device, unsigned rate, unsigned period)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    int err;
    unsigned periods = 3;
    snd_pcm_uframes_t psz = period, bsz = period * periods;

    /* The PipeWire and JACK ALSA plugins pick their own period size and
     * ignore the hw_params request.  They do honour this hint, so ask for
     * the latency we actually want before opening.  A value already in the
     * environment is the user's choice and is left alone. */
    char lat[32];
    snprintf(lat, sizeof lat, "%u/%u", period, rate);
    setenv("PIPEWIRE_LATENCY", lat, 0);

    if ((err = snd_pcm_open(&g_pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        snprintf(g_info, sizeof g_info, "alsa open '%s': %s", device, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(g_pcm, hw);
    snd_pcm_hw_params_set_access(g_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);

    if (snd_pcm_hw_params_set_format(g_pcm, hw, SND_PCM_FORMAT_FLOAT_LE) < 0) {
        snprintf(g_info, sizeof g_info, "alsa: float32 not supported");
        snd_pcm_close(g_pcm); g_pcm = NULL;
        return -1;
    }
    snd_pcm_hw_params_set_channels(g_pcm, hw, 2);
    snd_pcm_hw_params_set_rate_near(g_pcm, hw, &rate, 0);
    snd_pcm_hw_params_set_period_size_near(g_pcm, hw, &psz, 0);
    snd_pcm_hw_params_set_buffer_size_near(g_pcm, hw, &bsz);

    if ((err = snd_pcm_hw_params(g_pcm, hw)) < 0) {
        snprintf(g_info, sizeof g_info, "alsa hw_params: %s", snd_strerror(err));
        snd_pcm_close(g_pcm); g_pcm = NULL;
        return -1;
    }
    snd_pcm_hw_params_get_period_size(hw, &psz, 0);
    snd_pcm_hw_params_get_buffer_size(hw, &bsz);

    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(g_pcm, sw);
    snd_pcm_sw_params_set_start_threshold(g_pcm, sw, bsz - psz);
    snd_pcm_sw_params_set_avail_min(g_pcm, sw, psz);
    snd_pcm_sw_params(g_pcm, sw);

    if ((err = snd_pcm_prepare(g_pcm)) < 0) {
        snprintf(g_info, sizeof g_info, "alsa prepare: %s", snd_strerror(err));
        snd_pcm_close(g_pcm); g_pcm = NULL;
        return -1;
    }

    g_period = (unsigned)psz;
    g_engine = e;
    e->sr = (float)rate;
    atomic_store(&g_run, true);

    snprintf(g_info, sizeof g_info, "%s  %u Hz  %u frames (%.1f ms)  buf %u",
             device, rate, (unsigned)psz, 1000.0 * (double)psz / rate, (unsigned)bsz);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    struct sched_param sp = { .sched_priority = 70 };
    /* best effort: falls back to normal scheduling without RT privileges */
    if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0 &&
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO) == 0 &&
        pthread_attr_setschedparam(&attr, &sp) == 0 &&
        pthread_create(&g_thread, &attr, audio_loop, e) == 0) {
        pthread_attr_destroy(&attr);
        return 0;
    }
    pthread_attr_destroy(&attr);

    if (pthread_create(&g_thread, NULL, audio_loop, e) != 0) {
        snprintf(g_info, sizeof g_info, "cannot create audio thread");
        atomic_store(&g_run, false);
        snd_pcm_close(g_pcm); g_pcm = NULL;
        return -1;
    }
    return 0;
}

void audio_stop(void)
{
    if (!atomic_load(&g_run)) return;
    atomic_store(&g_run, false);
    pthread_join(g_thread, NULL);
    if (g_pcm) {
        snd_pcm_drop(g_pcm);
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
    }
    (void)g_engine;
}

/* ================================================================ *
 *  WAV writer (16 bit stereo)
 * ================================================================ */
struct WavWriter { FILE *f; long frames; int sr; };

static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

WavWriter *wav_open(const char *path, int sr)
{
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    WavWriter *w = calloc(1, sizeof *w);
    if (!w) { fclose(f); return NULL; }
    w->f = f; w->sr = sr;

    fwrite("RIFF", 1, 4, f); w32(f, 0); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(f, 16);
    w16(f, 1); w16(f, 2);
    w32(f, (uint32_t)sr);
    w32(f, (uint32_t)(sr * 4));
    w16(f, 4); w16(f, 16);
    fwrite("data", 1, 4, f); w32(f, 0);
    return w;
}

void wav_write(WavWriter *w, const float *in, int frames)
{
    if (!w) return;
    int16_t tmp[512];
    int n = frames * 2, i = 0;
    while (i < n) {
        int c = n - i > 512 ? 512 : n - i;
        for (int k = 0; k < c; k++) {
            float v = clampf(in[i + k], -1.0f, 1.0f);
            tmp[k] = (int16_t)lrintf(v * 32767.0f);
        }
        fwrite(tmp, 2, (size_t)c, w->f);
        i += c;
    }
    w->frames += frames;
}

long wav_frames(const WavWriter *w) { return w ? w->frames : 0; }

void wav_close(WavWriter *w)
{
    if (!w) return;
    uint32_t data = (uint32_t)(w->frames * 4);
    fseek(w->f, 4, SEEK_SET);  w32(w->f, 36 + data);
    fseek(w->f, 40, SEEK_SET); w32(w->f, data);
    fclose(w->f);
    free(w);
}

/* ================================================================ *
 *  Offline render - used by --render, and as a headless self test
 * ================================================================ */
int engine_render_file(Engine *e, const char *path, float seconds)
{
    const int    sr    = (int)e->sr;
    const int    block = 256;
    const long   total = (long)(seconds * sr);
    static const int chord[4][4] = {
        { 48, 55, 60, 64 },
        { 46, 53, 58, 62 },
        { 43, 50, 55, 59 },
        { 41, 48, 53, 57 },
    };

    WavWriter *w = wav_open(path, sr);
    if (!w) return -1;
    float *buf = malloc(sizeof(float) * (size_t)block * 2);
    if (!buf) { wav_close(w); return -1; }

    e->playing = true;
    long chordlen = (long)(seconds * sr / 4);
    int  cur = -1;

    for (long done = 0; done < total; done += block) {
        int idx = (int)(done / chordlen);
        if (idx > 3) idx = 3;
        if (idx != cur) {
            if (cur >= 0)
                for (int i = 0; i < 4; i++) engine_push(e, EV_OFF, chord[cur][i], 0);
            for (int i = 0; i < 4; i++) engine_push(e, EV_ON, chord[idx][i], 0.85f);
            cur = idx;
        }
        engine_render(e, buf, block);
        wav_write(w, buf, block);
    }
    for (int i = 0; i < 4; i++) engine_push(e, EV_OFF, chord[cur][i], 0);
    for (long done = 0; done < sr * 2L; done += block) {
        engine_render(e, buf, block);
        wav_write(w, buf, block);
    }

    long frames = wav_frames(w);
    free(buf);
    wav_close(w);
    return (int)(frames > 0 ? 0 : -1);
}
