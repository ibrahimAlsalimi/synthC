# synthc

A polyphonic subtractive synthesizer that lives in your terminal. Written in C,
ncurses front end, ALSA output.

```
  synthc 1.0    Fat Bass        oct C3   voices 3/4   cpu 6%   LATCH
  default  48000 Hz  1024 frames (21.3 ms)              L ▓▓▓░░░  R ▓▓░░░░
 ╭─ patch ──────────────────────────╮╭─ output ─────────────────────────────╮
 │  OSC  FILTER  ENV  LFO  FX  SEQ  ││ scope                        x4.1     │
 │ ▸Cutoff         620 Hz ▓▓▓▓▓░░░░ ││       │────     │──│     │────│       │
 │  Resonance        0.32 ▓▓▓░░░░░░ ││ ─│─│──│────────││─────────────│─│──── │
 │  Mode            LP 24 ░░░░░░░░░ ││  ││       ││  │        ─     │    ││  │
 │  Env Amount       0.55 ▓▓▓▓▓▓▓░░ ││ spectrum                              │
 │  Key Track        0.30 ▓▓▓░░░░░░ ││ ███████████████████▆████▁             │
 │  Drive           3.0 x ▓▓▓▓▓░░░░ ││ ██████████████████████████▃▄▅         │
 ╰──────────────────────────────────╯╰──────────────────────────────────────╯
      ▄  ▄     █  ▄  ▄     ▄  ▄     ▄  ▄  ▄
    ██▀▓▓▀▓▓ ▓▓█▓▓▀██▀▓▓ ▓▓▀▓▓▀▓▓ ▓▓▀▓▓▀▓▓▀▓▓
    C3                   C4
    seq C3   D3   E3   F#3  A3    ·   C4    ·
```

## Build

Needs `gcc` (or clang), ALSA and ncursesw development packages.

```sh
make
./synthc
```

Arch: `pacman -S alsa-lib ncurses`  ·  Debian/Ubuntu: `apt install libasound2-dev libncursesw5-dev`

`make install` puts it in `/usr/local/bin` (override with `PREFIX=`).

## Playing it

Your computer keyboard is the piano. Two rows, one octave each:

```
lower   z s x d c v g b h n j m , l . ; /     C  D  E  F  G  A  B  and a bit more
upper   q 2 w 3 e r 5 t 6 y 7 u i 9 o 0 p     the octave above
```

The black keys sit where they do on a real keyboard: `s` is C#, `d` is D#, and
so on. `[` and `]` move the whole map up and down an octave.

**A terminal never tells a program when a key is released.** synthc works around
this by watching your keyboard's auto-repeat: a key that stops repeating is
treated as released. It plays well, but if notes cut out and retrigger while you
hold them, your repeat delay is longer than the default window — either shorten
the repeat delay (`xset r rate 250 40` on X11, or the keyboard panel on
Wayland/GNOME/KDE) or raise the window with `--keyhold 900`. For sustained
chords, `Enter` toggles **latch**, which holds notes until you press them again.

## Controls

| | |
|---|---|
| `Tab` / `Shift-Tab` | next / previous parameter page |
| `↑` `↓` | select a parameter |
| `←` `→` | change it (`Shift`+`←`/`→` for fine steps) |
| `[` `]` | octave down / up |
| `-` `=` | master volume |
| `Enter` | latch held notes |
| `Space` | start / stop the sequencer transport |
| `8`, `F4` or `^T` | step record — every note you play writes a step and advances |
| `Backspace` | write a rest (while step recording) |
| `Delete` | clear the sequence |
| `4`, `F3` or `^A` | arpeggiator on / off |
| `1`, `F2` or `^P` | panic, all notes off |
| `PgUp` / `PgDn` | previous / next factory preset |
| `\` | randomize the patch |
| `'` | back to the Init patch |
| `F9` / `F10` | save / load a `.patch` file |
| `F5` or `^R` | start / stop writing a `.wav` of everything you play |
| `F1` or `?` | key map |
| `F12` or `Ctrl-C` | quit |

Shortcuts avoid the digits `2 3 5 6 7 9 0` because those are black keys.

## The synth

Sixteen voices, each with:

- **Two oscillators** — sine, triangle, saw, square, pulse, noise. Saw, square
  and pulse are PolyBLEP band-limited, so they stay clean up the keyboard
  (measured: no aliasing product above −42 dBc with the filter wide open).
  Plus a sub oscillator an octave down and a noise source.
- **A 4-pole ladder filter** in the zero-delay-feedback topology, with a
  saturating nonlinearity in the loop and Oberheim-style stage mixing for
  24/12 dB lowpass, 12/24 dB bandpass and 24 dB highpass. Self-oscillates.
- **Two ADSR envelopes** — one for amplitude, one for the filter, with a
  bipolar envelope amount and keyboard tracking.
- **An LFO** (sine/tri/saw/ramp/square/sample-and-hold) routable to pitch,
  cutoff, pulse width and amplitude.

Then, on the master bus: saturation, stereo chorus, a tempo-syncable ping-pong
delay with damping, and a Freeverb-style reverb. A soft limiter on the end means
nothing ever clips hard.

The **arpeggiator** runs up, down, up/down, down/up, random or as-played, over
one to four octaves, at any note division from 1/1 to 1/32 including triplets.
The **16-step sequencer** shares the same clock and has swing. Both are timed in
the audio thread, so steps land on the sample.

Nine factory presets (`--list-presets`), and `\` rolls a new random patch that
is usually musical and occasionally better than anything you'd have dialed in.

## Files

Patches are plain text — `key value` per line — so you can edit them by hand:

```
name Fat Bass
cut 620
res 0.32
...
```

`F5` records to `synthc-YYYYMMDD-HHMMSS.wav` in the working directory (16-bit
stereo). The audio thread writes into a lock-free ring that the UI thread
drains, so recording never disturbs playback.

## Without a sound card

```sh
./synthc --render out.wav 12        # renders a demo chord progression
./synthc -P 4 --render acid.wav 8   # ...using preset 4
```

Useful on a headless box, and it's how the DSP gets tested.

## Latency

synthc asks for a 256-frame period (~5 ms). PipeWire and JACK pick their own
period and will usually hand back 1024 frames (~21 ms), which is fine to play
but not tight. To go lower:

```sh
PIPEWIRE_LATENCY=256/48000 ./synthc     # per-application hint
./synthc -d plughw:0,0                  # talk to the card directly
```

The header line always shows what you actually got.

## Layout

```
src/synth.h    types, the Patch struct, declarations
src/dsp.c      envelopes, the ladder filter, delay lines
src/engine.c   oscillators, voice allocation, arp/sequencer clock, effects
src/params.c   the parameter table, factory presets, patch file I/O
src/audio.c    ALSA playback thread, WAV writer, offline rendering
src/ui.c       ncurses front end
```

Adding a parameter means adding one line to the table in `params.c` and one
field to `Patch` — the UI page, the value bar, the formatting and the preset
file format all follow from that.
