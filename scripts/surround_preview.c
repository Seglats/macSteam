/* Headphone surround's live preview: macSteam › Experimental › Headphone surround › Settings…
 * (SurroundSettings.swift, docs/SURROUND.md). Shipped as Contents/Helpers/surround_preview.
 *
 * The fitted model itself (scripts/surround_fit/render2.c: the head table reshaped by the knobs,
 * minimum-phase filters, group gains, reverb tail), played on the Mac's output with an AudioQueue,
 * so a changed knob is heard at once. Save bakes the knobs into the file Wine's driver loads for
 * games (wine-coreaudio-headphone-surround-settings.patch): the same filters, gains and tail.
 *
 *   surround_preview HEAD CLIP [knob=value ...] [--bake=PATH]
 *     HEAD: the head table (scripts/surround/kemar.head); CLIP: the ball bounce (a WAV)
 *     --bake=PATH: only bake PATH and exit, without playing (tests)
 *
 * Standard input: "play", "pause", "seek S", "gain X" (0-1), "surround 1|0" (0: the plain stereo mix
 * a game makes without headphone surround, as Wine's Off), "set knob=value ...", "file PATH" (a WAV,
 * 3 to 8 channels or stereo), "test" (back to the ball test), "bake PATH", "quit".
 * Standard output: "length S", "ready", "time S" (four times a second), "hit DEGREES PATTERN",
 * "file CHANNELS RATE SECONDS", "end" (played to the end), "baked PATH", "error TEXT".
 *
 * Build: clang -O2 -I scripts scripts/surround_preview.c -framework AudioToolbox -o surround_preview */
#define main render2_main
#include "surround_fit/render2.c"
#undef main
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#define RATE 48000
#define BUF_FRAMES 1024
#define LAT_FRAMES (3 * BUF_FRAMES)          /* what is queued ahead of what is heard */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static float *source;                         /* 7.1 frames at 48 kHz (a file), or NULL: the test */
static unsigned long long total, at_frame;    /* length and the next frame to render */
static struct sig seq;
static float *clip;
static size_t clip_frames;
static int playing, five_one, ended;
static volatile int fade_wanted, faded;       /* a knob change: fade out, change, fade back in */
static float env = 1, gain_target = 0.2f, gain_now = 0.2f, mix_target = 1, mix_now = 1;
static double seek_to = -1;
static int heard_hit = -1;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* ---- WAV files, mapped to 7.1 (FL FR FC LFE BL BR SL SR) at 48 kHz ---- */
static int channel_71(uint32_t bit)
{
    switch (bit) {
    case 0x1: case 0x40: case 0x1000: return 0;        /* front left, left of centre, top front left */
    case 0x2: case 0x80: case 0x4000: return 1;
    case 0x4: case 0x800: case 0x2000: return 2;       /* centre, top centre, top front centre */
    case 0x8: return 3;
    case 0x10: case 0x8000: return 4;                  /* back left, top back left */
    case 0x20: case 0x20000: return 5;
    case 0x200: return 6;                              /* side left */
    case 0x400: return 7;
    case 0x100: case 0x10000: return -1;               /* back centre: both backs */
    }
    return -2;
}

static float sample_at(const unsigned char *p, int bytes, int is_float)
{
    switch (bytes) {
    case 2: return (int16_t)(p[0] | p[1] << 8) / 32768.0f;
    case 3: return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0f;
    default: { uint32_t v = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; float f; if (is_float) { memcpy(&f, &v, 4); return f; } return (int32_t)v / 2147483648.0f; }
    }
}

/* The whole file as 7.1 at 48 kHz; NULL with *why set when it can't be read. */
static float *load_wav(const char *path, unsigned long long *frames_out, int *channels_out, int *rate_out, int *five_one_out, const char **why)
{
    FILE *f = fopen(path, "rb");
    unsigned char head[12], chunk[8], fmt[40] = {0}, *data = NULL;
    uint32_t size = 0, mask = 0, bit = 1, rate, channels, bytes, align;
    int have_fmt = 0, is_float = 0, map[18], c;
    float *out;
    unsigned long long frames, n, i;
    double step, pos;

    *why = "the file can't be opened";
    if (!f) return NULL;
    *why = "not a WAV file";
    if (fread(head, 1, 12, f) != 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) { fclose(f); return NULL; }
    for (;;) {
        if (fread(chunk, 1, 8, f) != 8) { *why = "no audio data in the file"; fclose(f); return NULL; }
        size = chunk[4] | chunk[5] << 8 | chunk[6] << 16 | (uint32_t)chunk[7] << 24;
        if (!memcmp(chunk, "fmt ", 4)) {
            if (fread(fmt, 1, size < sizeof(fmt) ? size : sizeof(fmt), f) < 16) { fclose(f); return NULL; }
            if (size > sizeof(fmt)) fseek(f, size - sizeof(fmt), SEEK_CUR);
            if (size & 1) fseek(f, 1, SEEK_CUR);
            have_fmt = 1;
        } else if (!memcmp(chunk, "data", 4)) break;
        else fseek(f, size + (size & 1), SEEK_CUR);
    }
    if (!have_fmt) { fclose(f); return NULL; }
    channels = fmt[2] | fmt[3] << 8;
    rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24;
    align = fmt[12] | fmt[13] << 8;
    bytes = (fmt[14] | fmt[15] << 8) / 8;
    is_float = (fmt[0] | fmt[1] << 8) == 3;
    if ((fmt[0] | fmt[1] << 8) == 0xFFFE) {
        mask = fmt[20] | fmt[21] << 8 | fmt[22] << 16 | (uint32_t)fmt[23] << 24;
        is_float = fmt[24] == 3;
    }
    *why = "unsupported WAV format (16, 24 or 32 bits, up to 18 channels)";
    if (!rate || !channels || channels > 18 || align != channels * bytes || (is_float ? bytes != 4 : (bytes < 2 || bytes > 4))) { fclose(f); return NULL; }
    if (!mask) mask = channels == 1 ? 0x4 : channels == 2 ? 0x3 : channels == 4 ? 0x33 : channels == 6 ? 0x3F : channels == 8 ? 0x63F : 0;
    for (c = 0; c < (int)channels; c++) {
        while (bit && !(mask & bit)) bit <<= 1;
        if (bit) { map[c] = channel_71(bit); bit <<= 1; } else map[c] = c < 8 ? c : -2;
    }
    frames = size / align;
    *why = "the file is too big to preview";
    if (frames > (unsigned long long)RATE * 60 * 30 || !(data = malloc(frames * align))) { fclose(f); return NULL; }
    frames = fread(data, align, frames, f);
    fclose(f);
    step = (double)rate / RATE;
    n = (unsigned long long)(frames / step);
    if (!(out = calloc(n * 8 + 8, sizeof(float)))) { free(data); return NULL; }
    for (i = 0, pos = 0; i < n; i++, pos += step) {
        unsigned long long j = (unsigned long long)pos;
        float t = (float)(pos - j);
        for (c = 0; c < (int)channels; c++) {
            float a = sample_at(data + j * align + c * bytes, bytes, is_float);
            float b = j + 1 < frames ? sample_at(data + (j + 1) * align + c * bytes, bytes, is_float) : a;
            float x = a + (b - a) * t;
            if (map[c] >= 0) out[i * 8 + map[c]] += x;
            else if (map[c] == -1) { out[i * 8 + 4] += 0.7071f * x; out[i * 8 + 5] += 0.7071f * x; }
        }
    }
    free(data);
    *frames_out = n; *channels_out = channels; *rate_out = rate;
    *five_one_out = (mask & 0x30) && !(mask & 0x600);  /* surrounds as backs, no sides: 5.1 */
    return out;
}

/* ---- the model from the knobs ---- */
/* 5.1's surrounds are rendered at the driver's 5.1 position (110 degrees, between side and back). */
static void configure_for_source(void)
{
    float back = knob("back"), el_back = knob("el_back"), g_back = knob("g_back");
    if (five_one) {
        set_knob("back", 110);
        set_knob("el_back", (knob("el_side") + el_back) / 2);
        set_knob("g_back", (knob("g_side") + g_back) / 2);
    }
    configure();
    if (five_one) { set_knob("back", back); set_knob("el_back", el_back); set_knob("g_back", g_back); }
}

/* Changes the model without a click: the output fades out over a buffer, the filters change, it fades back in. */
static void reconfigure(void)
{
    int waited;
    fade_wanted = 1; faded = 0;
    for (waited = 0; !faded && waited < 100 && playing; waited++) usleep(2000);
    pthread_mutex_lock(&lock);
    configure_for_source();
    pthread_mutex_unlock(&lock);
    fade_wanted = 0;
}

/* The baked file for Wine: "SDSURR1\0", rate, taps, positions (12), then per position its gain,
 * the LFE gain, the tail (level, time, damping, start), then per position both ears' filters. */
static int bake(const char *path)
{
    float az[12], el[12], g[12], v;
    static struct filt f;
    char tmp[4096];
    uint32_t head[3] = {RATE, SPK_TAPS + 48, 12};
    FILE *o;
    int d, e;
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if (!(o = fopen(tmp, "wb"))) return 0;
    positions(az, el, g);
    fwrite("SDSURR1", 1, 8, o);
    fwrite(head, 4, 3, o);
    for (d = 0; d < 12; d++) { v = knob("gain") * powf(10, g[d] / 20); fwrite(&v, 4, 1, o); }
    v = 0.25f; fwrite(&v, 4, 1, o);
    v = knob("tail"); fwrite(&v, 4, 1, o);
    v = knob("rt"); fwrite(&v, 4, 1, o);
    v = knob("tail_lp"); fwrite(&v, 4, 1, o);
    v = knob("tail_ms"); fwrite(&v, 4, 1, o);
    for (d = 0; d < 12; d++) {
        make(az[d], el[d], &f, 1, SPK_TAPS);
        for (e = 0; e < 2; e++) fwrite(f.h[e], 4, SPK_TAPS + 48, o);
    }
    if (fclose(o) != 0) return 0;
    return rename(tmp, path) == 0;
}

/* ---- playback ---- */
static void render(float *out, int n)
{
    int i;
    pthread_mutex_lock(&lock);
    if (seek_to >= 0) {
        at_frame = (unsigned long long)(seek_to * RATE);
        if (at_frame > total) at_frame = total;
        seek_to = -1; seq.next = 0; heard_hit = -1; ended = 0;
    }
    for (i = 0; i < n; i++) {
        float in[8] = {0}, on[2], off[2];
        int live = playing && at_frame < total;
        if (live) {
            if (source) memcpy(in, source + at_frame * 8, sizeof(in));
            else sig_frame(&seq, at_frame, in);
            at_frame++;
        }
        frame(in, on);                         /* always: the history and tail run on, so switching is seamless */
        off[0] = in[0] + 0.7071f * (in[2] + in[4] + in[6]);
        off[1] = in[1] + 0.7071f * (in[2] + in[5] + in[7]);
        mix_now += (mix_target - mix_now) * 0.004f;
        gain_now += ((playing ? gain_target : 0) - gain_now) * 0.004f;
        if (fade_wanted) { env -= 1.0f / (n * 0.8f); if (env <= 0) { env = 0; faded = 1; } }
        else if (env < 1) { env += 1.0f / n; if (env > 1) env = 1; }
        out[2 * i] = (mix_now * on[0] + (1 - mix_now) * off[0]) * gain_now * env;
        out[2 * i + 1] = (mix_now * on[1] + (1 - mix_now) * off[1]) * gain_now * env;
    }
    if (playing && at_frame >= total && !ended) { ended = 1; playing = 0; say("end"); }
    pthread_mutex_unlock(&lock);
}

static void callback(void *user, AudioQueueRef q, AudioQueueBufferRef b)
{
    (void)user;
    render((float *)b->mAudioData, BUF_FRAMES);
    b->mAudioDataByteSize = BUF_FRAMES * 8;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static void use_test(void)
{
    source = NULL; five_one = 0;
    sig_init(&seq, clip, clip_frames, RATE);
    total = seq.total; at_frame = 0; ended = 0; heard_hit = -1;
}

static void *reporter(void *arg)
{
    double last = -1;
    (void)arg;
    for (;;) {
        usleep(50000);
        pthread_mutex_lock(&lock);
        long long heard = (long long)at_frame - (playing ? LAT_FRAMES : 0);
        int hit = -1;
        if (heard < 0) heard = 0;
        if (!source && playing && (unsigned long long)heard < seq.total) hit = sig_hit_at(&seq, heard);
        pthread_mutex_unlock(&lock);
        double t = (double)heard / RATE;
        if (fabs(t - last) >= 0.25) { say("time %.2f", t); last = t; }
        if (hit >= 0 && hit != heard_hit) { heard_hit = hit; say("hit %.1f %d", seq.hit[hit].az, seq.hit[hit].pass + 1); }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    AudioStreamBasicDescription fmt = {RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AudioQueueRef q;
    pthread_t thread;
    char line[4096];
    const char *why;
    unsigned long long frames;
    int channels, rate, fo, i;
    float *clip71;
    const char *bake_only = NULL;

    if (argc < 3) { fprintf(stderr, "usage: surround_preview HEAD CLIP [knob=value ...]\n"); return 2; }
    if (!load_head(argv[1])) { say("error the head table can't be read"); return 1; }
    for (i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!strncmp(argv[i], "--bake=", 7)) { bake_only = argv[i] + 7; continue; }
        if (eq) { *eq = 0; if (!set_knob(argv[i], (float)atof(eq + 1))) { say("error no knob %s", argv[i]); return 1; } }
    }
    if (bake_only) {                     /* tests and scripts: bake and leave, no audio */
        configure();
        if (!bake(bake_only)) { say("error the settings couldn't be saved"); return 1; }
        say("baked %s", bake_only);
        return 0;
    }
    if (!(clip71 = load_wav(argv[2], &frames, &channels, &rate, &fo, &why))) { say("error the test sound: %s", why); return 1; }
    clip = malloc(frames * sizeof(float));
    for (unsigned long long k = 0; k < frames; k++) clip[k] = clip71[k * 8 + 2];   /* mono: the centre */
    clip_frames = frames;
    free(clip71);
    use_test();
    configure_for_source();
    say("length %.1f", (double)total / RATE);

    if (AudioQueueNewOutput(&fmt, callback, NULL, NULL, NULL, 0, &q)) { say("error no audio output"); return 1; }
    for (i = 0; i < 3; i++) {
        AudioQueueBufferRef b;
        AudioQueueAllocateBuffer(q, BUF_FRAMES * 8, &b);
        callback(NULL, q, b);
    }
    if (AudioQueueStart(q, NULL)) { say("error the audio output didn't start"); return 1; }
    pthread_create(&thread, NULL, reporter, NULL);
    say("ready");

    while (fgets(line, sizeof(line), stdin)) {
        char *nl = strchr(line, '\n'), *arg;
        double t;
        float g;
        int on;
        if (nl) *nl = 0;
        if ((nl = strchr(line, '\r'))) *nl = 0;
        arg = strchr(line, ' ');
        if (!strcmp(line, "play")) { pthread_mutex_lock(&lock); if (at_frame >= total) { seek_to = 0; } playing = 1; ended = 0; pthread_mutex_unlock(&lock); }
        else if (!strcmp(line, "pause")) { pthread_mutex_lock(&lock); playing = 0; pthread_mutex_unlock(&lock); }
        else if (sscanf(line, "seek %lf", &t) == 1 && t >= 0) { pthread_mutex_lock(&lock); seek_to = t; pthread_mutex_unlock(&lock); }
        else if (sscanf(line, "gain %f", &g) == 1 && g >= 0 && g <= 1) gain_target = g;
        else if (sscanf(line, "surround %d", &on) == 1) mix_target = on ? 1 : 0;
        else if (!strncmp(line, "set ", 4)) {
            char *p = line + 4, *tok;
            while ((tok = strsep(&p, " "))) {
                char *eq = strchr(tok, '=');
                if (eq) { *eq = 0; if (!set_knob(tok, (float)atof(eq + 1))) say("error no knob %s", tok); }
            }
            reconfigure();
        }
        else if (!strncmp(line, "file ", 5) && arg) {
            float *data = load_wav(arg + 1, &frames, &channels, &rate, &fo, &why);
            if (!data) { say("error %s", why); continue; }
            pthread_mutex_lock(&lock);
            playing = 0;
            free(source); source = data; total = frames; at_frame = 0; ended = 0; five_one = fo;
            configure_for_source();
            pthread_mutex_unlock(&lock);
            say("file %d %d %.1f", channels, rate, (double)frames / RATE);
            say("length %.1f", (double)frames / RATE);
        }
        else if (!strcmp(line, "test")) {
            pthread_mutex_lock(&lock);
            playing = 0; free(source); use_test(); configure_for_source();
            pthread_mutex_unlock(&lock);
            say("length %.1f", (double)total / RATE);
        }
        else if (!strncmp(line, "bake ", 5) && arg) {
            int ok;
            pthread_mutex_lock(&lock);       /* make() shares buffers with nothing the player uses, but knobs stay put */
            ok = bake(arg + 1);
            pthread_mutex_unlock(&lock);
            if (ok) say("baked %s", arg + 1); else say("error the settings couldn't be saved");
        }
        else if (!strcmp(line, "quit")) break;
    }
    AudioQueueStop(q, true);
    return 0;
}
