/* main.c - command line, engine bring-up */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "synth.h"

static void usage(const char *argv0)
{
    printf(
"%s %s - a polyphonic subtractive synthesizer for the terminal\n"
"\n"
"usage: %s [options]\n"
"\n"
"  -d, --device NAME    ALSA device (default: \"default\")\n"
"  -r, --rate HZ        sample rate (default: 48000)\n"
"  -p, --period N       period size in frames, lower = less latency (default: 256)\n"
"  -P, --preset N       start on factory preset N\n"
"  -k, --keyhold MS     how long a key counts as held before the terminal's\n"
"                       auto-repeat starts (default 620; lower it if your\n"
"                       keyboard repeat delay is short)\n"
"  -l, --load FILE      start from a saved .patch file\n"
"      --render FILE S  render S seconds of a demo chord progression to FILE\n"
"                       and exit (no audio device needed)\n"
"      --list-presets   print the factory presets and exit\n"
"  -h, --help           this message\n"
"\n"
"Once running press F1 for the key map.\n", APP_NAME, APP_VERSION, argv0);
}

int main(int argc, char **argv)
{
    const char *device = "default";
    const char *load = NULL, *render = NULL;
    unsigned rate = 48000, period = 256;
    float rsecs = 12.0f;
    int preset = 0, keyhold = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int has = (i + 1 < argc);
        if      ((!strcmp(a, "-d") || !strcmp(a, "--device")) && has) device = argv[++i];
        else if ((!strcmp(a, "-r") || !strcmp(a, "--rate")) && has)   rate = (unsigned)atoi(argv[++i]);
        else if ((!strcmp(a, "-p") || !strcmp(a, "--period")) && has) period = (unsigned)atoi(argv[++i]);
        else if ((!strcmp(a, "-P") || !strcmp(a, "--preset")) && has) preset = atoi(argv[++i]);
        else if ((!strcmp(a, "-l") || !strcmp(a, "--load")) && has)   load = argv[++i];
        else if ((!strcmp(a, "-k") || !strcmp(a, "--keyhold")) && has) keyhold = atoi(argv[++i]);
        else if (!strcmp(a, "--render") && has) {
            render = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') rsecs = (float)atof(argv[++i]);
        }
        else if (!strcmp(a, "--list-presets")) {
            Patch p;
            for (int k = 0; k < patch_preset_count(); k++) {
                patch_load_preset(&p, k);
                printf("%2d  %s\n", k, p.name);
            }
            return 0;
        }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return 2; }
    }

    if (rate < 8000 || rate > 192000) rate = 48000;
    if (period < 32 || period > 8192) period = 256;

    Engine *e = calloc(1, sizeof *e);
    if (!e) { fprintf(stderr, "out of memory\n"); return 1; }
    engine_init(e, (float)rate);
    patch_load_preset(&e->patch, preset);
    if (load && patch_load_file(&e->patch, load) != 0)
        fprintf(stderr, "warning: could not read %s\n", load);

    if (render) {
        printf("rendering %.1fs of \"%s\" to %s ...\n", rsecs, e->patch.name, render);
        int rc = engine_render_file(e, render, rsecs);
        engine_free(e);
        free(e);
        if (rc != 0) { fprintf(stderr, "render failed\n"); return 1; }
        printf("done\n");
        return 0;
    }

    if (audio_start(e, device, rate, period) != 0) {
        fprintf(stderr, "audio: %s\n", audio_info());
        fprintf(stderr,
            "\nNo audio output. Things to try:\n"
            "  - list devices:   aplay -L\n"
            "  - pick one:       %s -d plughw:0,0\n"
            "  - render a file:  %s --render out.wav 10\n", argv[0], argv[0]);
        engine_free(e);
        free(e);
        return 1;
    }

    if (keyhold > 0) ui_set_keyhold(keyhold);
    ui_run(e);

    audio_stop();
    engine_free(e);
    free(e);
    return 0;
}
