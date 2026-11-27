/* Headphone surround test (macSteam › Experimental › Headphone surround, Surround.swift,
 * docs/SURROUND.md). Run with the SteamDriver tool's Wine in a prefix of its own; never part of
 * a runtime. It plays through the same path a game's sound takes: a shared-mode IAudioClient
 * stream in 7.1 (FL FR FC LFE BL BR SL SR), whatever the device's mix format: with
 * WINE_HEADPHONE_SURROUND set, Wine's headphone rendering. Without it (macSteam's Off), a stereo
 * stream of the same sound downmixed here, as a game does for a stereo output (see -stereo).
 *
 *   surround_test.exe -test CLIP.wav   the test: CLIP (the ball bounce, scripts/surround/) at
 *                                      different spots around you, five patterns (surround_signal.h)
 *   surround_test.exe FILE.wav         a WAV file (PCM 16/24/32-bit or 32-bit float, 1-8 channels)
 *   -gain X first: volume, 0 to 1 (macSteam's slider, 0.2 to start); -quiet is 0.001 (tests)
 *   -record OUT.wav (before -test or the file): also records what the output device plays
 *   (WASAPI loopback, the device's mix format, 32-bit float), from just before the first frame
 *   to the end. On Windows with Dolby Atmos for Headphones or Windows Sonic on the device, that
 *   is their headphone rendering of the same 7.1: a reference to compare (docs/SURROUND.md).
 *   -start S: begin S seconds in.
 *   -sync NAME -voice on|off [-off]: macSteam runs two players, one with headphone surround and
 *   one without, on one timeline (a shared memory block NAME): both always play the same place,
 *   locked to the clock, and only the one switched to is heard, so On/Off swaps without a gap.
 *   -off: Off is heard first. With -sync they start paused (wait for "play"); alone, at once.
 *   The Off voice (or -stereo) plays a stereo stream with its own standard downmix of the 7.1
 *   (each side: front + 0.7071 x centre, side and back; no LFE), what a game makes for a stereo
 *   output. Handing a stereo device 7.1 instead, macOS matches channels by speaker and drops
 *   centre, sides and backs (heard 2026-11-27): no game without headphone surround does that.
 *   On standard input while it plays: "gain X" (volume), "seek S" (jump to S seconds), "pause",
 *   "play", "surround 1|0" (which voice is heard). With -sync one player takes them for both.
 *
 * Output lines for macSteam: "mix <channels> <mask> <rate>", "length <seconds>", "stream <channels>"
 * (what this player plays: 8, or 2 for the Off voice), "ready" (the
 * stream runs), "hit <degrees> <pattern 1-5>" when a hit of the test is heard (from the stream
 * clock), "file <channels> <rate> <seconds>", "time <seconds>" (four times a second, test and
 * file), "done", or "error <text>". */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include "surround_signal.h"

#define CHANNELS 8
#define MASK_71 (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | \
                 SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)

static void fail(const char *what, HRESULT hr);

/* ---- the timeline: shared by the On and Off players (-sync), or this player's own ----
 * Every change (play, pause, seek, which voice is heard) takes effect `lead` after it's asked
 * for, later than anything already queued, so each player can place it at the same sample. */
struct timeline
{
    volatile LONG lock;
    LONG version;       /* changes with every change */
    LONG jump;          /* changes with play and seek: the source position starts over */
    LONG playing;       /* 0 = paused (zero, so a block not yet set up is paused) */
    LONG off;           /* 1 = the Off voice is heard */
    LONG64 epoch;       /* QPC time in 100 ns when the latest change is heard */
    double base;        /* source seconds heard at epoch */
    float gain;
};
static struct timeline *tl;
static LONG64 lead = 2000000;   /* 100 ns; set from the queue's length */

static LONG64 now_100ns(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (LONG64)((double)c.QuadPart * 1e7 / (double)f.QuadPart);
}
static void tl_lock(void) { while (InterlockedCompareExchange(&tl->lock, 1, 0)) Sleep(0); }
static void tl_unlock(void) { InterlockedExchange(&tl->lock, 0); }

static void command(const char *line)
{
    float g; double t; int on;
    LONG64 at; double here;
    tl_lock();
    if (sscanf(line, "gain %f", &g) == 1) { if (g >= 0 && g <= 1) tl->gain = g; tl_unlock(); return; }
    at = now_100ns() + lead;
    /* the source heard at `at` on the timeline as it is */
    here = tl->playing ? tl->base + (double)(at - tl->epoch) / 1e7 : tl->base;
    if (here < 0) here = 0;
    if (!strncmp(line, "play", 4)) { if (tl->playing) goto done; tl->playing = 1; tl->jump++; }
    else if (!strncmp(line, "pause", 5)) { if (!tl->playing) goto done; tl->playing = 0; }
    else if (sscanf(line, "seek %lf", &t) == 1 && t >= 0) { here = t; tl->jump++; }
    else if (sscanf(line, "surround %d", &on) == 1) tl->off = !on;
    else goto done;
    tl->base = here; tl->epoch = at; tl->version++;
done:
    tl_unlock();
}

/* ---- -record: loopback capture of the output device ---- */
struct recorder
{
    IAudioClient *ac; IAudioCaptureClient *cc; FILE *f;
    UINT32 channels, rate, frames; volatile LONG stop; HANDLE thread;
};

static void write_wav_header(FILE *f, UINT32 channels, UINT32 rate, UINT32 frames)
{
    WAVEFORMATEXTENSIBLE w; UINT32 v;
    memset(&w, 0, sizeof(w));
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; w.Format.nChannels = channels; w.Format.nSamplesPerSec = rate;
    w.Format.wBitsPerSample = 32; w.Format.nBlockAlign = channels * 4; w.Format.nAvgBytesPerSec = rate * channels * 4;
    w.Format.cbSize = 22; w.Samples.wValidBitsPerSample = 32;
    w.dwChannelMask = channels == 2 ? KSAUDIO_SPEAKER_STEREO : channels == 8 ? MASK_71 : 0;
    w.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); v = 4 + 8 + sizeof(w) + 8 + frames * channels * 4; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = sizeof(w); fwrite(&v, 4, 1, f); fwrite(&w, sizeof(w), 1, f);
    fwrite("data", 1, 4, f); v = frames * channels * 4; fwrite(&v, 4, 1, f);
}

static DWORD WINAPI record_loop(void *arg)
{
    struct recorder *r = arg;
    while (!r->stop) {
        UINT32 packet = 0;
        Sleep(5);
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(r->cc, &packet)) && packet) {
            BYTE *data; UINT32 n; DWORD flags; UINT32 i;
            if (FAILED(IAudioCaptureClient_GetBuffer(r->cc, &data, &n, &flags, NULL, NULL))) break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) { float zero = 0; for (i = 0; i < n * r->channels; i++) fwrite(&zero, 4, 1, r->f); }
            else fwrite(data, 4 * r->channels, n, r->f);
            r->frames += n;
            IAudioCaptureClient_ReleaseBuffer(r->cc, n);
        }
    }
    return 0;
}

static void record_start(struct recorder *r, IMMDevice *dev, const char *path)
{
    WAVEFORMATEX *mix; HRESULT hr;
    memset(r, 0, sizeof(*r));
    if (FAILED(hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&r->ac))) fail("can't record the output", hr);
    IAudioClient_GetMixFormat(r->ac, &mix);
    if (mix->wBitsPerSample != 32) fail("the output's mix format isn't 32-bit float", 0);
    r->channels = mix->nChannels; r->rate = mix->nSamplesPerSec;
    if (FAILED(hr = IAudioClient_Initialize(r->ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix, NULL)))
        fail("can't record the output (loopback)", hr);
    IAudioClient_GetService(r->ac, &IID_IAudioCaptureClient, (void **)&r->cc);
    if (!(r->f = fopen(path, "wb"))) fail("can't write the recording", 0);
    write_wav_header(r->f, r->channels, r->rate, 0);
    IAudioClient_Start(r->ac);
    r->thread = CreateThread(NULL, 0, record_loop, r, 0, NULL);
    printf("record %u %u\n", r->channels, r->rate);
}

static void record_stop(struct recorder *r)
{
    if (!r->f) return;
    Sleep(400);   /* the tail of the last sound */
    r->stop = 1;
    WaitForSingleObject(r->thread, INFINITE);
    IAudioClient_Stop(r->ac);
    write_wav_header(r->f, r->channels, r->rate, r->frames);
    fclose(r->f);
    printf("recorded %.1f\n", (double)r->frames / r->rate);
}

static DWORD WINAPI read_input(void *arg)
{
    char line[64];
    while (fgets(line, sizeof(line), stdin)) command(line);
    return 0;
}

static void fail(const char *what, HRESULT hr)
{
    printf("error %s (0x%08lx)\n", what, (unsigned long)hr);
    fflush(stdout);
    exit(1);
}

/* ---- WAV files ---- */
struct wav
{
    FILE *f;
    UINT32 rate, channels, bytes, frame_bytes, is_float;
    UINT64 frames, read_frames;
    long data_start;    /* file offset of the first frame */
    int map[18];        /* source channel -> 7.1 channel, -1 = both backs, -2 = dropped */
    BYTE *buf; UINT32 buf_frames, buf_have, buf_pos;
    float prev[CHANNELS], next[CHANNELS];
    double pos;         /* between prev (0) and next (1) */
    BOOL ended;
};

static int channel_71(DWORD bit)
{
    switch (bit) {
    case SPEAKER_FRONT_LEFT: case SPEAKER_FRONT_LEFT_OF_CENTER: case SPEAKER_TOP_FRONT_LEFT: return 0;
    case SPEAKER_FRONT_RIGHT: case SPEAKER_FRONT_RIGHT_OF_CENTER: case SPEAKER_TOP_FRONT_RIGHT: return 1;
    case SPEAKER_FRONT_CENTER: case SPEAKER_TOP_CENTER: case SPEAKER_TOP_FRONT_CENTER: return 2;
    case SPEAKER_LOW_FREQUENCY: return 3;
    case SPEAKER_BACK_LEFT: case SPEAKER_TOP_BACK_LEFT: return 4;
    case SPEAKER_BACK_RIGHT: case SPEAKER_TOP_BACK_RIGHT: return 5;
    case SPEAKER_SIDE_LEFT: return 6;
    case SPEAKER_SIDE_RIGHT: return 7;
    case SPEAKER_BACK_CENTER: case SPEAKER_TOP_BACK_CENTER: return -1;
    }
    return -2;
}

static void wav_open(struct wav *w, const char *path)
{
    BYTE head[12], chunk[8];
    WAVEFORMATEXTENSIBLE fmt;
    BOOL have_fmt = FALSE;
    DWORD mask = 0, bit = 1;
    UINT32 c;

    memset(w, 0, sizeof(*w));
    if (!(w->f = fopen(path, "rb"))) fail("the file can't be opened", 0);
    if (fread(head, 1, 12, w->f) != 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4))
        fail("not a WAV file", 0);
    for (;;) {
        UINT32 size;
        if (fread(chunk, 1, 8, w->f) != 8) fail("no audio data in the file", 0);
        size = chunk[4] | chunk[5] << 8 | chunk[6] << 16 | (UINT32)chunk[7] << 24;
        if (!memcmp(chunk, "fmt ", 4)) {
            memset(&fmt, 0, sizeof(fmt));
            if (fread(&fmt, 1, min(size, sizeof(fmt)), w->f) < 16) fail("damaged WAV header", 0);
            if (size > sizeof(fmt)) fseek(w->f, size - sizeof(fmt), SEEK_CUR);
            if (size & 1) fseek(w->f, 1, SEEK_CUR);
            have_fmt = TRUE;
        } else if (!memcmp(chunk, "data", 4)) {
            if (!have_fmt) fail("damaged WAV header", 0);
            w->frames = fmt.Format.nBlockAlign ? size / fmt.Format.nBlockAlign : 0;
            w->data_start = ftell(w->f);
            break;
        } else {
            fseek(w->f, size + (size & 1), SEEK_CUR);
        }
    }
    w->rate = fmt.Format.nSamplesPerSec;
    w->channels = fmt.Format.nChannels;
    w->bytes = fmt.Format.wBitsPerSample / 8;
    w->frame_bytes = fmt.Format.nBlockAlign;
    if (fmt.Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) w->is_float = 1;
    else if (fmt.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        w->is_float = IsEqualGUID(&fmt.SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        if (!w->is_float && !IsEqualGUID(&fmt.SubFormat, &KSDATAFORMAT_SUBTYPE_PCM)) fail("unsupported WAV encoding", 0);
        mask = fmt.dwChannelMask;
    } else if (fmt.Format.wFormatTag != WAVE_FORMAT_PCM) fail("unsupported WAV encoding (only PCM and float)", 0);
    if (!w->rate || !w->channels || w->channels > 18 || (w->is_float ? w->bytes != 4 : (w->bytes < 2 || w->bytes > 4)) ||
        w->frame_bytes != w->channels * w->bytes)
        fail("unsupported WAV format (16, 24 or 32 bits, up to 18 channels)", 0);
    if (!mask) mask = w->channels == 1 ? SPEAKER_FRONT_CENTER : w->channels == 2 ? KSAUDIO_SPEAKER_STEREO :
                      w->channels == 4 ? KSAUDIO_SPEAKER_QUAD : w->channels == 6 ? KSAUDIO_SPEAKER_5POINT1 :
                      w->channels == 8 ? MASK_71 : 0;
    for (c = 0; c < w->channels; c++) {
        while (bit && !(mask & bit)) bit <<= 1;
        if (bit) { w->map[c] = channel_71(bit); bit <<= 1; }
        else w->map[c] = c < CHANNELS ? (int)c : -2;
    }
    w->buf_frames = 4096;
    w->buf = malloc(w->buf_frames * w->frame_bytes);
}

static float wav_sample(const struct wav *w, const BYTE *p)
{
    switch (w->bytes) {
    case 2: return *(const INT16 *)p / 32768.0f;
    case 3: return (INT32)((UINT32)p[0] << 8 | (UINT32)p[1] << 16 | (UINT32)p[2] << 24) / 2147483648.0f;
    default: return w->is_float ? *(const float *)p : *(const INT32 *)p / 2147483648.0f;
    }
}

/* The next source frame, mapped to 7.1; FALSE at the end. */
static BOOL wav_next(struct wav *w, float *out)
{
    const BYTE *frame;
    UINT32 c;

    memset(out, 0, CHANNELS * sizeof(float));
    if (w->buf_pos == w->buf_have) {
        UINT64 left = w->frames - w->read_frames;
        UINT32 want = (UINT32)min((UINT64)w->buf_frames, left);
        w->buf_have = want ? (UINT32)fread(w->buf, w->frame_bytes, want, w->f) : 0;
        w->buf_pos = 0;
        w->read_frames += w->buf_have;
        if (!w->buf_have) return FALSE;
    }
    frame = w->buf + w->buf_pos++ * w->frame_bytes;
    for (c = 0; c < w->channels; c++) {
        float x = wav_sample(w, frame + c * w->bytes);
        if (w->map[c] >= 0) out[w->map[c]] += x;
        else if (w->map[c] == -1) { out[4] += x * 0.7071f; out[5] += x * 0.7071f; }
    }
    return TRUE;
}

/* Continue from source frame `frame` (a jump). */
static void wav_seek(struct wav *w, UINT64 frame)
{
    if (frame > w->frames) frame = w->frames;
    fseek(w->f, w->data_start + (long)(frame * w->frame_bytes), SEEK_SET);
    w->read_frames = frame;
    w->buf_have = w->buf_pos = 0;
    w->pos = 1.0;
    w->ended = FALSE;
}

/* One output frame at the device rate (linear interpolation); FALSE at the end. */
static BOOL wav_frame(struct wav *w, UINT32 rate, float *out)
{
    UINT32 c;
    if (w->ended) return FALSE;
    while (w->pos >= 1.0) {
        memcpy(w->prev, w->next, sizeof(w->prev));
        if (!wav_next(w, w->next)) { w->ended = TRUE; return FALSE; }
        w->pos -= 1.0;
    }
    for (c = 0; c < CHANNELS; c++) out[c] = w->prev[c] + (w->next[c] - w->prev[c]) * (float)w->pos;
    w->pos += (double)w->rate / rate;
    return TRUE;
}

int main(int argc, char **argv)
{
    IMMDeviceEnumerator *en; IMMDevice *dev; IAudioClient *ac; IAudioRenderClient *rc; IAudioClock *clock;
    WAVEFORMATEX *mix; WAVEFORMATEXTENSIBLE fmt;
    struct sig seq; struct wav wav; BOOL file, test = FALSE; float *clip = NULL;
    struct recorder rec; const char *record = NULL;
    UINT32 size, pad, rate; UINT64 written = 0, freq, pos, qpc, end_frame = 0, total;
    INT64 offset = 0;   /* source frame = stream frame + offset (jumps change it) */
    INT64 seg_start = 0;   /* the source frame the latest jump went to */
    int heard = -1; double last_time = -1, start = 0, est = 0;
    float gain = 1.0f, g = 0, k;
    const char *sync = NULL; LONG me_off = 0, start_off = 0, applied_version = -1, applied_jump = -1;
    BOOL created = TRUE, have_clock = FALSE, playing = FALSE, audible = TRUE;
    BOOL stereo = FALSE;   /* the Off voice: a stereo stream, downmixed here */
    UINT32 nch;
    struct timeline now_tl;
    struct { BOOL set, jump, playing, audible; double at; INT64 offset; } next = {0};
    HRESULT hr;

    setvbuf(stdout, NULL, _IONBF, 0);   /* the CRT has no line buffering: _IOLBF is full buffering */
    while (argc > 1 && argv[1][0] == '-') {
        const char *a = argv[1], *v = argc > 2 ? argv[2] : NULL;
        if (!strcmp(a, "-test")) { test = TRUE; argv++; argc--; break; }
        if (!strcmp(a, "-quiet")) gain = 0.001f;
        else if (!strcmp(a, "-off")) start_off = 1;
        else if (v && !strcmp(a, "-gain")) { gain = (float)atof(v); if (gain < 0 || gain > 1) gain = 0.2f; }
        else if (v && !strcmp(a, "-record")) record = v;
        else if (v && !strcmp(a, "-start")) start = atof(v);
        else if (v && !strcmp(a, "-sync")) sync = v;
        else if (v && !strcmp(a, "-voice")) me_off = !strcmp(v, "off");
        else if (!strcmp(a, "-stereo")) { stereo = TRUE; argv++; argc--; continue; }
        else fail("unknown option", 0);
        if (!strcmp(a, "-quiet") || !strcmp(a, "-off")) { argv++; argc--; } else { argv += 2; argc -= 2; }
    }
    if (argc < 2) fail("give -test CLIP.wav or a WAV file", 0);
    file = !test;
    if (sync) {
        HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(*tl), sync);
        created = GetLastError() != ERROR_ALREADY_EXISTS;
        if (!m || !(tl = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*tl)))) fail("no shared timeline", 0);
    } else tl = calloc(1, sizeof(*tl));
    if (created) { tl_lock(); tl->base = start; tl->off = start_off; tl->gain = gain; tl->version++; tl_unlock(); }
    CloseHandle(CreateThread(NULL, 0, read_input, NULL, 0, NULL));
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en)))
        fail("no audio devices", hr);
    if (FAILED(hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) fail("no audio output", hr);
    if (FAILED(hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac))) fail("audio output unavailable", hr);
    if (FAILED(hr = IAudioClient_GetMixFormat(ac, &mix))) fail("no mix format", hr);
    rate = mix->nSamplesPerSec;
    printf("mix %u 0x%lx %u\n", mix->nChannels,
           mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? (unsigned long)((WAVEFORMATEXTENSIBLE *)mix)->dwChannelMask : 0ul, rate);

    if (file) {
        wav_open(&wav, argv[1]);
        wav.pos = 1.0;
        printf("file %u %u %.1f\n", wav.channels, wav.rate, (double)wav.frames / wav.rate);
    } else {
        /* the clip, mono at the output's rate (the WAV reader maps mono to the center channel) */
        UINT64 frames = 0, cap;
        float f71[CHANNELS];
        wav_open(&wav, argv[1]);
        wav.pos = 1.0;
        cap = wav.frames * rate / wav.rate + 16;
        clip = malloc(cap * sizeof(float));
        while (frames < cap && wav_frame(&wav, rate, f71)) clip[frames++] = f71[2];
        sig_init(&seq, clip, frames, rate);
    }
    /* the length at the output's rate, in frames */
    total = file ? (UINT64)((double)wav.frames * rate / wav.rate) : seq.total;
    printf("length %.1f\n", (double)total / rate);
    k = 1.0f - expf(-1.0f / (0.005f * rate));   /* volume changes glide over about 5 ms: no clicks */

    memset(&fmt, 0, sizeof(fmt));
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    if (sync && me_off) stereo = TRUE;
    nch = stereo ? 2 : CHANNELS;
    fmt.Format.nChannels = nch;
    fmt.Format.nSamplesPerSec = rate;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = nch * 4;
    fmt.Format.nAvgBytesPerSec = rate * nch * 4;
    fmt.Format.cbSize = sizeof(fmt) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = stereo ? KSAUDIO_SPEAKER_STEREO : MASK_71;
    fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 1000000, 0, &fmt.Format, NULL);
    if (FAILED(hr)) fail(stereo ? "the output doesn't take a stereo stream" : "the output doesn't take a 7.1 stream", hr);
    printf("stream %u\n", nch);   /* 8: the 7.1 a game plays; 2: the Off voice's stereo mix */
    IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    IAudioClient_GetService(ac, &IID_IAudioClock, (void **)&clock);
    IAudioClock_GetFrequency(clock, &freq);
    IAudioClient_GetBufferSize(ac, &size);
    lead = (LONG64)size * 10000000 / rate + 600000;   /* the queue plus 60 ms */
    memset(&rec, 0, sizeof(rec));
    if (record) record_start(&rec, dev, record);

    for (;;) {
        UINT32 n, i; BYTE *data; BOOL more = TRUE;
        double posf;
        /* the stream clock: est is the stream position (in 100 ns) minus the QPC time, the
           largest seen (the position moves in steps; just after a step is the true one). Wine's
           own position time is from another clock than QueryPerformanceCounter, so it's taken here. */
        IAudioClock_GetPosition(clock, &pos, &qpc);
        qpc = (UINT64)now_100ns();
        posf = freq ? (double)pos * rate / freq : 0;
        if (posf > 0) {
            double e = posf * 1e7 / rate - (double)qpc;
            if (!have_clock) { have_clock = TRUE; est = e; printf("ready\n"); if (!sync) command("play"); }
            else est = e > est ? e : est - 10;   /* forgets 0.2 ms a second: clock drift */
        }
        tl_lock(); now_tl = *tl; tl_unlock();
        if (have_clock && now_tl.version != applied_version) {
            /* the stream frame heard at the change, and the source from there on */
            applied_version = now_tl.version;
            next.at = ((double)now_tl.epoch + est) * rate / 1e7;
            next.offset = (INT64)floor(now_tl.base * rate - next.at + 0.5);
            next.jump = (next.set && next.jump) || now_tl.jump != applied_jump;
            applied_jump = now_tl.jump;
            next.playing = now_tl.playing;
            next.audible = !sync || now_tl.off == me_off;
            next.set = TRUE;
        }
        IAudioClient_GetCurrentPadding(ac, &pad);
        n = size - pad;
        if (n && !end_frame) {
            if (FAILED(IAudioRenderClient_GetBuffer(rc, n, &data))) break;
            for (i = 0; i < n; i++) {
                float *out = (float *)data + i * nch, f71[CHANNELS];
                UINT64 p = written + i;
                float target;
                int c;
                if (next.set && (double)p >= next.at) {
                    next.set = FALSE;
                    if (next.jump) {
                        INT64 src = (INT64)p + next.offset;
                        if (src < 0) src = 0;
                        offset = src - (INT64)p;
                        if (file) wav_seek(&wav, (UINT64)((double)src * wav.rate / rate));
                        else seq.next = 0;   /* sig_frame finds its place again, also going back */
                        seg_start = src;
                        heard = -1;
                    }
                    playing = next.playing; audible = next.audible;
                }
                target = playing && audible ? now_tl.gain : 0;
                g += (target - g) * k;
                if (!playing && g < 1e-5f) { g = 0; memset(out, 0, nch * sizeof(float)); continue; }
                more = file ? wav_frame(&wav, rate, f71) : sig_frame(&seq, (UINT64)((INT64)p + offset), f71);
                if (!more) { memset(out, 0, (n - i) * nch * sizeof(float)); end_frame = p; break; }
                if (stereo) {      /* FL FR FC LFE BL BR SL SR -> L R */
                    out[0] = f71[0] + 0.7071f * (f71[2] + f71[4] + f71[6]);
                    out[1] = f71[1] + 0.7071f * (f71[2] + f71[5] + f71[7]);
                } else memcpy(out, f71, sizeof(f71));
                for (c = 0; c < nch; c++) out[c] *= g;
            }
            IAudioRenderClient_ReleaseBuffer(rc, n, 0);
            written += n;
            if (written == n) IAudioClient_Start(ac);
        }
        /* what is being heard now */
        pos = (UINT64)posf;
        if (have_clock) {
            /* the source position heard now (just after a jump, the queued part is counted as
               from the new place) */
            INT64 src = (INT64)pos + offset;
            BOOL running = playing && now_tl.playing;
            double t = running ? (double)src / rate : now_tl.base;
            if ((!running || src >= seg_start) && fabs(t - last_time) >= 0.25) { printf("time %.2f\n", t); last_time = t; }
            if (running && !file && src >= seg_start && (UINT64)src < seq.total) {
                int hit = sig_hit_at(&seq, (UINT64)src);
                if (hit >= 0 && hit != heard) { heard = hit; printf("hit %.1f %d\n", seq.hit[hit].az, seq.hit[hit].pass + 1); }
            }
        }
        if (end_frame && pos >= end_frame) break;
        Sleep(5);
    }
    record_stop(&rec);
    IAudioClient_Stop(ac);
    printf("done\n");
    return 0;
}
