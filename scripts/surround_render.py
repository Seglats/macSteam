#!/usr/bin/env python3
"""Headphone surround without Wine: the Wine patches' own binaural model (the code they add to
winecoreaudio.drv, taken at run time from environments/finals/patches/
wine-coreaudio-headphone-surround.patch and the -fitted.patch after it, filters included, so this
can't drift from what the runtime does) compiled for the Mac and run on files.

    surround_render.py test [DIR]        macSteam's test as files: the ball bounce
                                         (scripts/surround/ball-bounce.wav) at different spots
                                         around you, in five patterns (scripts/surround_signal.h):
                                         test-7.1.wav        the 7.1 a game would play
                                         test-headphones.wav what headphone surround makes of it
                                         test-plain.wav      a plain stereo downmix (surround off)
                                         test-hits.txt       each hit: time, direction, pattern
    surround_render.py render IN OUT     any multichannel WAV through the model to stereo
    surround_render.py cut SOURCE START LENGTH OUT
                                         one sound cut from a recording (any format ffmpeg reads)
                                         as a test clip: mono, 48 kHz, rumble below 80 Hz removed,
                                         a 1 ms fade in and a fade out over its last 60 %, peak at
                                         0.6 so the headphone rendering has room. Made
                                         scripts/surround/ball-bounce.wav (README.md there).

DIR defaults to ~/Library/Application Support/SteamDriver/surround-test/files. Play the files in
QuickTime with headphones: this checks the model alone, before Wine, Core Audio or a game.
docs/SURROUND.md, "Testing ladder"."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'environments/finals/patches/wine-coreaudio-headphone-surround.patch'
FITTED = ROOT / 'environments/finals/patches/wine-coreaudio-headphone-surround-fitted.patch'
SETTINGS = ROOT / 'environments/finals/patches/wine-coreaudio-headphone-surround-settings.patch'
SIGNAL = ROOT / 'scripts/surround_signal.h'
CLIP = ROOT / 'scripts/surround/ball-bounce.wav'

PRELUDE = r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL; typedef unsigned int UINT; typedef uint32_t DWORD, UINT32; typedef int16_t INT16; typedef int32_t INT32;
typedef unsigned char BYTE;
#define TRUE 1
#define FALSE 0
#define TRACE(...) do {} while (0)
#define WARN(...) do {} while (0)
typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } GUID;
static GUID KSDATAFORMAT_SUBTYPE_PCM = {1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
static int IsEqualGUID(const GUID *a, const GUID *b) { return !memcmp(a, b, sizeof(GUID)); }
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_IEEE_FLOAT 3
#define WAVE_FORMAT_EXTENSIBLE 0xfffe
#pragma pack(push, 1)
typedef struct { uint16_t wFormatTag, nChannels; uint32_t nSamplesPerSec, nAvgBytesPerSec; uint16_t nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX;
typedef struct { WAVEFORMATEX Format; union { uint16_t wValidBitsPerSample; } Samples; uint32_t dwChannelMask; GUID SubFormat; } WAVEFORMATEXTENSIBLE;
#pragma pack(pop)
enum { SPEAKER_FRONT_LEFT = 1, SPEAKER_FRONT_RIGHT = 2, SPEAKER_FRONT_CENTER = 4, SPEAKER_LOW_FREQUENCY = 8,
       SPEAKER_BACK_LEFT = 0x10, SPEAKER_BACK_RIGHT = 0x20, SPEAKER_FRONT_LEFT_OF_CENTER = 0x40,
       SPEAKER_FRONT_RIGHT_OF_CENTER = 0x80, SPEAKER_BACK_CENTER = 0x100, SPEAKER_SIDE_LEFT = 0x200,
       SPEAKER_SIDE_RIGHT = 0x400, SPEAKER_TOP_CENTER = 0x800, SPEAKER_TOP_FRONT_LEFT = 0x1000,
       SPEAKER_TOP_FRONT_CENTER = 0x2000, SPEAKER_TOP_FRONT_RIGHT = 0x4000, SPEAKER_TOP_BACK_LEFT = 0x8000,
       SPEAKER_TOP_BACK_CENTER = 0x10000, SPEAKER_TOP_BACK_RIGHT = 0x20000 };
static DWORD get_channel_mask(unsigned int c)
{
    switch (c) { case 1: return 4; case 2: return 3; case 4: return 0x33; case 6: return 0x3f; case 8: return 0x63f; }
    return 0;
}
'''

MAIN = r'''
#include "surround_signal.h"

static void wav_header(FILE *f, unsigned channels, unsigned rate, unsigned long long frames)
{
    WAVEFORMATEXTENSIBLE fmt; uint32_t v;
    memset(&fmt, 0, sizeof(fmt));
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; fmt.Format.nChannels = channels; fmt.Format.nSamplesPerSec = rate;
    fmt.Format.wBitsPerSample = 32; fmt.Format.nBlockAlign = channels * 4; fmt.Format.nAvgBytesPerSec = rate * channels * 4;
    fmt.Format.cbSize = 22; fmt.Samples.wValidBitsPerSample = 32; fmt.dwChannelMask = channels == 8 ? 0x63f : 3;
    fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    fwrite("RIFF", 1, 4, f); v = (uint32_t)(4 + 8 + sizeof(fmt) + 8 + frames * channels * 4); fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = sizeof(fmt); fwrite(&v, 4, 1, f); fwrite(&fmt, sizeof(fmt), 1, f);
    fwrite("data", 1, 4, f); v = (uint32_t)(frames * channels * 4); fwrite(&v, 4, 1, f);
}

static FILE *create(const char *dir, const char *name)
{
    char path[4096]; FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(f = fopen(path, "wb"))) { perror(path); exit(1); }
    printf("%s\n", path);
    return f;
}

/* the clip as raw mono float at 48 kHz */
static void test_files(const char *raw, const char *dir)
{
    const unsigned rate = 48000;
    FILE *in = fopen(raw, "rb"), *f71, *fhp, *fpl, *hits; float *clip, frame[8], out[2], plain[2], peak = 0;
    long bytes; unsigned long long frames, n; WAVEFORMATEXTENSIBLE fmt; struct binaural *b; struct sig s; int i;
    if (!in) { perror(raw); exit(1); }
    fseek(in, 0, SEEK_END); bytes = ftell(in); fseek(in, 0, SEEK_SET);
    frames = bytes / sizeof(float); clip = malloc(bytes);
    if (fread(clip, sizeof(float), frames, in) != frames) exit(1);
    fclose(in);
    sig_init(&s, clip, frames, rate);
    f71 = create(dir, "test-7.1.wav"); fhp = create(dir, "test-headphones.wav"); fpl = create(dir, "test-plain.wav");
    hits = create(dir, "test-hits.txt");
    for (i = 0; i < s.hits; i++) fprintf(hits, "%.3f %.1f %d %s\n", s.hit[i].t, s.hit[i].az, s.hit[i].pass + 1, sig_pass_names[s.hit[i].pass]);
    memset(&fmt, 0, sizeof(fmt));
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; fmt.Format.nChannels = 8; fmt.Format.nSamplesPerSec = rate;
    fmt.Format.wBitsPerSample = 32; fmt.Format.nBlockAlign = 32; fmt.dwChannelMask = 0x63f; fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    b = binaural_create(&fmt.Format);
    wav_header(f71, 8, rate, s.total); wav_header(fhp, 2, rate, s.total); wav_header(fpl, 2, rate, s.total);
    for (n = 0; sig_frame(&s, n, frame); n++) {
        binaural_frame(b, (const BYTE *)frame, out);
        peak = fmaxf(peak, fmaxf(fabsf(out[0]), fabsf(out[1])));
        /* plain stereo downmix, no head model */
        plain[0] = 0.5f * (frame[0] + 0.7071f * (frame[2] + frame[4] + frame[6])) + 0.25f * frame[3];
        plain[1] = 0.5f * (frame[1] + 0.7071f * (frame[2] + frame[5] + frame[7])) + 0.25f * frame[3];
        fwrite(frame, sizeof(float), 8, f71); fwrite(out, sizeof(float), 2, fhp); fwrite(plain, sizeof(float), 2, fpl);
    }
    fclose(f71); fclose(fhp); fclose(fpl); fclose(hits); free(clip); binaural_free(b);
    fprintf(stderr, "peak %.2f%s\n", peak, peak > 1 ? " (over 1.0: clips)" : "");
}

static void render(const char *in, const char *outpath)
{
    FILE *f = fopen(in, "rb"), *o; BYTE head[12], chunk[8], *frame; WAVEFORMATEXTENSIBLE fmt;
    uint32_t size = 0; int have_fmt = 0; struct binaural *b; float out[2]; unsigned long long frames, n;
    if (!f) { perror(in); exit(1); }
    if (fread(head, 1, 12, f) != 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) { fprintf(stderr, "not a WAV file\n"); exit(1); }
    for (;;) {
        if (fread(chunk, 1, 8, f) != 8) { fprintf(stderr, "no audio data\n"); exit(1); }
        size = chunk[4] | chunk[5] << 8 | chunk[6] << 16 | (uint32_t)chunk[7] << 24;
        if (!memcmp(chunk, "fmt ", 4)) {
            memset(&fmt, 0, sizeof(fmt));
            if (fread(&fmt, 1, size < sizeof(fmt) ? size : sizeof(fmt), f) < 16) exit(1);
            if (size > sizeof(fmt)) fseek(f, size - sizeof(fmt), SEEK_CUR);
            if (size & 1) fseek(f, 1, SEEK_CUR);
            have_fmt = 1;
        } else if (!memcmp(chunk, "data", 4)) break;
        else fseek(f, size + (size & 1), SEEK_CUR);
    }
    if (!have_fmt || fmt.Format.nChannels <= 2 || fmt.Format.nChannels > BIN_MAX_CHANNELS) {
        fprintf(stderr, "needs a WAV with 3 to %d channels (stereo is left alone by headphone surround)\n", BIN_MAX_CHANNELS); exit(1);
    }
    if (!(b = binaural_create(&fmt.Format)) || !b->bytes) { fprintf(stderr, "unsupported sample format\n"); exit(1); }
    frames = size / fmt.Format.nBlockAlign;
    if (!(o = fopen(outpath, "wb"))) { perror(outpath); exit(1); }
    wav_header(o, 2, fmt.Format.nSamplesPerSec, frames);
    frame = malloc(fmt.Format.nBlockAlign);
    for (n = 0; n < frames && fread(frame, fmt.Format.nBlockAlign, 1, f) == 1; n++) {
        binaural_frame(b, frame, out);
        fwrite(out, sizeof(float), 2, o);
    }
    fclose(o); fclose(f); free(frame); binaural_free(b);
    printf("%s\n", outpath);
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "test")) { test_files(argv[2], argv[3]); return 0; }
    if (argc == 4 && !strcmp(argv[1], "render")) { render(argv[2], argv[3]); return 0; }
    fprintf(stderr, "usage: test CLIP.f32 DIR | render IN OUT\n");
    return 2;
}
'''


def patched(patch):
    """The patched side of every hunk (added and context lines), in order."""
    return '\n'.join(line[1:] for line in patch.read_text().splitlines() if line[:1] in '+ ' and not line.startswith('+++'))


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def model_source():
    """The C the patches add to coreaudio.c, cut to the binaural model (with the context lines a
    diff shares with the original, such as a closing brace). The first patch's sample reading,
    then the newest patch's filters and rendering: the fitted patch's, which replace the first
    patch's sphere, as the settings patch leaves them (it reads WINE_HEADPHONE_SURROUND_FILTERS,
    a file macSteam bakes; without it, the fitted filters)."""
    first = patched(PATCH)
    newest = patched(SETTINGS) if SETTINGS.exists() else patched(FITTED)
    return '\n'.join([
        between(newest, '#include "surround_ir.h"', 'static BOOL headphone_surround'),
        between(first, '/* Sample size in bytes', '/* Whether a render stream'),
        between(newest, 'static enum surround_pos binaural_position', 'static float binaural_sample'),
        between(first, 'static float binaural_sample', '/* One frame of the stream'),
        between(newest, '/* One frame of the stream', 'static NTSTATUS unix_process_attach'),
    ])


def filters():
    """surround_ir.h, the new file the fitted patch adds."""
    text = FITTED.read_text()
    body = text[text.index('+++ b/dlls/winecoreaudio.drv/surround_ir.h'):].splitlines()[2:]
    return '\n'.join(line[1:] for line in body if line.startswith('+')) + '\n'


def build(folder):
    source = folder / 'surround_render.c'
    source.write_text(PRELUDE + model_source() + MAIN)
    (folder / 'surround_ir.h').write_text(filters())
    tool = folder / 'surround_render'
    subprocess.run(['/usr/bin/clang', '-O2', '-w', '-I', str(folder), '-I', str(SIGNAL.parent), str(source), '-o', str(tool), '-lm'], check=True)
    return tool


def raw_mono(source, folder):
    raw = Path(folder) / 'clip.f32'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(source), '-ac', '1', '-ar', '48000', '-f', 'f32le', str(raw)], check=True)
    return raw


def players(out):
    """The files as players take them: 24-bit PCM instead of 32-bit float (Music and others don't
    play float WAV)."""
    for name in ('test-7.1', 'test-headphones', 'test-plain'):
        wav = out / f'{name}.wav'
        tmp = out / f'.{name}.tmp.wav'
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(wav), '-c:a', 'pcm_s24le', str(tmp)], check=True)
        tmp.replace(wav)


def cut(source, start, length, out):
    """One sound from a recording, cleaned, as a 16-bit mono 48 kHz WAV with its peak at 0.6."""
    start, length = float(start), float(length)
    with tempfile.TemporaryDirectory(prefix='surround-render-') as folder:
        raw = Path(folder) / 'cut.f32'
        fade = length * 0.6
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', str(start), '-t', str(length), '-i', str(source),
                        '-ac', '1', '-ar', '48000',
                        '-af', f'highpass=f=80,afade=t=in:d=0.001,afade=t=out:st={length - fade:.4f}:d={fade:.4f}',
                        '-f', 'f32le', str(raw)], check=True)
        data = raw.read_bytes()
        import struct
        values = struct.unpack('<%df' % (len(data) // 4), data)
        scale = 0.6 / max(abs(v) for v in values)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'f32le', '-ar', '48000', '-ac', '1', '-i', str(raw),
                        '-af', f'volume={scale:.4f}', '-c:a', 'pcm_s16le', '-map_metadata', '-1', '-bitexact', str(out)], check=True)
    print(out)
    return 0


def main(argv):
    default = Path.home() / 'Library/Application Support/SteamDriver/surround-test/files'
    if len(argv) >= 1 and argv[0] == 'test':
        out = Path(argv[1]) if len(argv) > 1 else default
        out.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='surround-render-') as folder:
            done = subprocess.run([str(build(Path(folder))), 'test', str(raw_mono(CLIP, folder)), str(out)]).returncode
        if done == 0:
            players(out)
        return done
    if len(argv) == 3 and argv[0] == 'render':
        with tempfile.TemporaryDirectory(prefix='surround-render-') as folder:
            return subprocess.run([str(build(Path(folder))), 'render', argv[1], argv[2]]).returncode
    if len(argv) == 5 and argv[0] == 'cut':
        return cut(*argv[1:])
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
