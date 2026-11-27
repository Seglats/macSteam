/* Headphone surround with measured head responses (experimental, docs/SURROUND.md): each speaker
 * of a multichannel stream is convolved with MIT's KEMAR head-related impulse responses for its
 * direction (scripts/surround/kemar_elev0.h), which carry the ear-shape cues for front and back
 * that a spherical head model lacks. With room on, three early reflections per speaker (the near
 * side wall, the far side wall, the front or back wall) arrive from their own directions through
 * the same responses, and a short, quiet reverb tail follows: sound then seems to come from
 * outside the head.
 *
 * Plain C in the style of the Wine patch (wine-coreaudio-headphone-surround.patch), so a version
 * that sounds right can move there as it is. Rendered offline by scripts/surround_render.py. */
#ifndef SURROUND_HRTF_H
#define SURROUND_HRTF_H

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "surround/kemar_elev0.h"

#define HRTF_MAX_CHANNELS 18
#define HRTF_ROOM_DIRS 12                 /* reflections arrive from every 30 degrees */
#define HRTF_SOURCES (HRTF_MAX_CHANNELS + HRTF_ROOM_DIRS)
#define HRTF_MAX_TAPS 1500                /* 128 taps at 44.1 kHz (320 shaped), up to 192 kHz */
#define HRTF_HIST 16384                   /* history per source, power of two: taps + 40 ms at 192 kHz */
#define HRTF_REFLECTIONS 3
#define HRTF_FDN 4
#define HRTF_EQ_TAPS 511                  /* front equalization, linear phase */

struct hrtf_ir { int taps; float h[2][HRTF_MAX_TAPS]; };

/* Direction cues, each 1 = as measured (hrtf_shape): itd scales how much later the far ear hears a
 * sound, ild how much quieter it is (per frequency), contrast how far each direction's tone is
 * from the average over all directions (where front and back differ). */
struct hrtf_cues { float itd, ild, contrast; };

#define HRTF_FFT 512
/* In place, radix 2; inverse divides by n. */
static void hrtf_fft(float *re, float *im, int n, int inverse)
{
    int i, j, len;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (len = 2; len <= n; len <<= 1) {
        float a = 2 * (float)M_PI / len * (inverse ? 1 : -1);
        for (i = 0; i < n; i += len)
            for (j = 0; j < len / 2; j++) {
                float wr = cosf(a * j), wi = sinf(a * j);
                float ur = re[i + j], ui = im[i + j];
                float vr = re[i + j + len / 2] * wr - im[i + j + len / 2] * wi;
                float vi = re[i + j + len / 2] * wi + im[i + j + len / 2] * wr;
                re[i + j] = ur + vr; im[i + j] = ui + vi;
                re[i + j + len / 2] = ur - vr; im[i + j + len / 2] = ui - vi;
            }
    }
    if (inverse) for (i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

/* A response's level per FFT bin, in dB (bins 0 .. HRTF_FFT/2). */
static void hrtf_level(const short *h, float *db)
{
    float re[HRTF_FFT] = {0}, im[HRTF_FFT] = {0};
    int k;
    for (k = 0; k < KEMAR_TAPS; k++) re[k] = h[k] / 32768.0f;
    hrtf_fft(re, im, HRTF_FFT, 0);
    for (k = 0; k <= HRTF_FFT / 2; k++) db[k] = 10 * log10f(re[k] * re[k] + im[k] * im[k] + 1e-12f);
}

/* The minimum-phase response with this level per bin (real cepstrum), HRTF_FFT samples. */
static void hrtf_minphase(const float *db, float *h)
{
    float re[HRTF_FFT], im[HRTF_FFT];
    int k;
    for (k = 0; k < HRTF_FFT; k++) {
        int b = k <= HRTF_FFT / 2 ? k : HRTF_FFT - k;
        re[k] = db[b] * (float)M_LN10 / 20; im[k] = 0;
    }
    hrtf_fft(re, im, HRTF_FFT, 1);                     /* cepstrum */
    for (k = 1; k < HRTF_FFT / 2; k++) { re[k] *= 2; im[k] *= 2; }
    for (k = HRTF_FFT / 2 + 1; k < HRTF_FFT; k++) re[k] = im[k] = 0;
    hrtf_fft(re, im, HRTF_FFT, 0);
    for (k = 0; k < HRTF_FFT; k++) {
        float m = expf(re[k]);
        re[k] = m * cosf(im[k]); im[k] = m * sinf(im[k]);
    }
    hrtf_fft(re, im, HRTF_FFT, 1);
    for (k = 0; k < HRTF_FFT; k++) h[k] = re[k];
}

/* When an ear first hears the sound: the first sample within 20 dB of the response's peak. */
static int hrtf_onset(const short *h)
{
    int k, peak = 0;
    for (k = 0; k < KEMAR_TAPS; k++) if (abs(h[k]) > peak) peak = abs(h[k]);
    for (k = 0; k < KEMAR_TAPS; k++) if (abs(h[k]) * 10 >= peak) return k;
    return 0;
}

struct hrtf
{
    int channels, room, eq;
    float rate;
    unsigned int pos;
    int lfe[HRTF_MAX_CHANNELS];
    float gain[HRTF_MAX_CHANNELS];
    struct hrtf_ir ir[HRTF_SOURCES];      /* channels first, then the room directions */
    float hist[HRTF_SOURCES][HRTF_HIST];
    /* room: each channel's reflections (delay in samples, gain, room direction) */
    int refl_delay[HRTF_MAX_CHANNELS][HRTF_REFLECTIONS], refl_dir[HRTF_MAX_CHANNELS][HRTF_REFLECTIONS];
    float refl_gain[HRTF_MAX_CHANNELS][HRTF_REFLECTIONS];
    float room_lp[HRTF_ROOM_DIRS], lp_k;
    /* reverb tail: a four-line feedback delay network fed by all speakers */
    float *fdn[HRTF_FDN];
    int fdn_len[HRTF_FDN], fdn_pos[HRTF_FDN];
    float fdn_damp[HRTF_FDN], fdn_g, fdn_k;
    int tail_delay;
    float room_gain, tail_gain;
    /* front equalization: the same filter on both outputs (it is the same for every direction) */
    float eq_h[HRTF_EQ_TAPS], eq_hist[2][1024];
    unsigned int eq_pos;
};

/* A filter that undoes the front response's tone (the average of both ears at 0 degrees), so a
 * sound straight ahead keeps its own tone and only the differences between directions remain,
 * as Dolby's renderer does (measured, docs/SURROUND.md). Smoothed over a third of an octave,
 * held within +-12 dB, and flat above 16 kHz. */
static void hrtf_front_eq(struct hrtf *m, const struct hrtf_ir *front)
{
    enum { N = 1024 };
    static float mag[N / 2 + 1], smooth[N / 2 + 1];
    int k, n;
    for (k = 0; k <= N / 2; k++) {
        float re[2] = {0, 0}, im[2] = {0, 0};
        int e;
        for (e = 0; e < 2; e++)
            for (n = 0; n < front->taps; n++) {
                float a = -2 * (float)M_PI * k * n / N;
                re[e] += front->h[e][n] * cosf(a);
                im[e] += front->h[e][n] * sinf(a);
            }
        mag[k] = sqrtf((re[0] * re[0] + im[0] * im[0] + re[1] * re[1] + im[1] * im[1]) / 2) + 1e-6f;
    }
    for (k = 0; k <= N / 2; k++) {
        float f = (float)k * m->rate / N, lo = f / 1.1225f, hi = f * 1.1225f, sum = 0, g;
        int j, count = 0;
        for (j = 0; j <= N / 2; j++) {
            float fj = (float)j * m->rate / N;
            if (fj >= lo && fj <= hi) { sum += mag[j] * mag[j]; count++; }
        }
        g = count ? 1 / sqrtf(sum / count) : 1 / mag[k];
        if (g > 3.98f) g = 3.98f;
        if (g < 0.251f) g = 0.251f;
        if (f > 16000) g = 1;
        smooth[k] = g;
    }
    /* linear phase: real, symmetric impulse response from the magnitudes, Hann-windowed */
    for (n = 0; n < HRTF_EQ_TAPS; n++) {
        float t = n - (HRTF_EQ_TAPS - 1) / 2.0f, sum = smooth[0] + smooth[N / 2] * cosf((float)M_PI * t);
        for (k = 1; k < N / 2; k++) sum += 2 * smooth[k] * cosf(2 * (float)M_PI * k * t / N);
        m->eq_h[n] = sum / N * (0.5f - 0.5f * cosf(2 * (float)M_PI * n / (HRTF_EQ_TAPS - 1)));
    }
}

/* The KEMAR response for a direction (degrees clockwise from the front) at rate, resampled with
 * a windowed sinc and scaled so the response's level doesn't depend on the rate. With cues, the
 * response is first split into each ear's onset and a minimum-phase filter, the cues are scaled
 * (struct hrtf_cues; avg: the level per bin averaged over all directions and ears), and the
 * onsets are put back with the timing difference scaled. */
static void hrtf_load(struct hrtf_ir *ir, float az, float rate, float norm, const struct hrtf_cues *cues, const float *avg)
{
    int index, swap, n, k, e;
    float ratio = KEMAR_RATE / rate, fc = rate < KEMAR_RATE ? rate / KEMAR_RATE : 1.0f;
    static float shaped[2][HRTF_FFT];
    float delay[2] = {0, 0};
    int src_taps = KEMAR_TAPS;

    az = fmodf(az, 360.0f);
    if (az < 0) az += 360.0f;
    swap = az > 180;
    index = (int)floorf((swap ? 360 - az : az) / 5 + 0.5f);
    if (index >= KEMAR_STEPS) index = KEMAR_STEPS - 1;
    if (cues) {
        static float db[2][HRTF_FFT / 2 + 1];
        const short *h[2] = {kemar_elev0[index][swap ? 1 : 0], kemar_elev0[index][swap ? 0 : 1]};
        int onset[2], first;
        for (e = 0; e < 2; e++) { hrtf_level(h[e], db[e]); onset[e] = hrtf_onset(h[e]); }
        for (k = 0; k <= HRTF_FFT / 2; k++) {
            float mono = (db[0][k] + db[1][k]) / 2, mono_new = avg[k] + cues->contrast * (mono - avg[k]);
            for (e = 0; e < 2; e++) db[e][k] = mono_new + cues->ild * (db[e][k] - mono);
        }
        first = onset[0] < onset[1] ? onset[0] : onset[1];
        for (e = 0; e < 2; e++) {
            hrtf_minphase(db[e], shaped[e]);
            delay[e] = first + cues->itd * (onset[e] - first);
        }
        src_taps = 320;
    }
    ir->taps = (int)ceilf((src_taps + (delay[0] > delay[1] ? delay[0] : delay[1])) / ratio) + 8;
    if (ir->taps > HRTF_MAX_TAPS) ir->taps = HRTF_MAX_TAPS;
    for (e = 0; e < 2; e++) {
        const short *h = kemar_elev0[index][swap ? 1 - e : e];
        for (n = 0; n < ir->taps; n++) {
            float t = n * ratio - delay[e], sum = 0;
            for (k = 0; k < src_taps; k++) {
                float x = fc * (t - k), w, s;
                if (fabsf(x) >= 16) continue;
                s = x == 0 ? 1.0f : sinf((float)M_PI * x) / ((float)M_PI * x);
                w = 0.5f + 0.5f * cosf((float)M_PI * x / 16);
                sum += (cues ? shaped[e][k] * 32768.0f : h[k]) * s * w;
            }
            ir->h[e][n] = sum * ratio * fc * norm / 32768.0f;
        }
    }
}

static float hrtf_azimuth(unsigned int bit, unsigned int mask)
{
    int sides = (mask & 0x600) != 0, backs = (mask & 0x30) != 0;
    switch (bit) {
    case 0x1: case 0x1000: return -30;
    case 0x2: case 0x4000: return 30;
    case 0x40: return -15;
    case 0x80: return 15;
    case 0x10: case 0x8000: return sides ? -150 : -110;
    case 0x20: case 0x20000: return sides ? 150 : 110;
    case 0x100: case 0x10000: return 180;
    case 0x200: return backs ? -90 : -110;
    case 0x400: return backs ? 90 : 110;
    }
    return 0;
}

/* channels in Windows order for the speaker mask; early and tail: the levels of the early
 * reflections and the reverb tail (0 none, 1 = the first version's); size: the room's size, 1 =
 * about 5 x 4 m with a 0.4 s reverb (walls, reverb lines and reverb time all scale with it, the
 * tail's start with its square root; 1.8 was far too big, docs/SURROUND.md); eq: front
 * equalization (hrtf_front_eq). */
static struct hrtf *hrtf_create(int channels, unsigned int mask, float rate, float early, float tail, float size, int eq,
                                const struct hrtf_cues *cues)
{
    static const int fdn_ms[HRTF_FDN] = {29, 37, 41, 47};   /* tenths are added below */
    struct hrtf *m;
    unsigned int bit = 1;
    float norm = 0;
    int c, i, a, e;

    static float avg[HRTF_FFT / 2 + 1];
    if (channels > HRTF_MAX_CHANNELS || !(m = calloc(1, sizeof(*m)))) return NULL;
    if (cues) {
        /* the level per bin averaged over every direction and both ears */
        float db[HRTF_FFT / 2 + 1];
        memset(avg, 0, sizeof(avg));
        for (a = 0; a < KEMAR_STEPS; a++)
            for (e = 0; e < 2; e++) {
                hrtf_level(kemar_elev0[a][e], db);
                for (i = 0; i <= HRTF_FFT / 2; i++) avg[i] += db[i] / (KEMAR_STEPS * 2);
            }
    }
    m->channels = channels;
    m->rate = rate;
    m->room = early > 0 || tail > 0;
    m->room_gain = early;
    m->tail_gain = tail;
    m->eq = eq;
    /* diffuse-field level: the average power over all directions and both ears is 1 */
    for (a = 0; a < KEMAR_STEPS; a++)
        for (e = 0; e < 2; e++)
            for (i = 0; i < KEMAR_TAPS; i++) norm += (kemar_elev0[a][e][i] / 32768.0f) * (kemar_elev0[a][e][i] / 32768.0f);
    norm = 1 / sqrtf(norm / (KEMAR_STEPS * 2));

    for (c = 0; c < channels; c++) {
        unsigned int this_bit = 0;
        float az;
        while (bit && !(mask & bit)) bit <<= 1;
        if (bit) { this_bit = bit; bit <<= 1; }
        if (this_bit == 0x8) { m->lfe[c] = 1; m->gain[c] = 0.25f; continue; }
        az = hrtf_azimuth(this_bit, mask);
        m->gain[c] = 0.35f;   /* diffuse-field level is about 6 dB above the sphere model's: match it */
        hrtf_load(&m->ir[c], az, rate, norm, cues, avg);
        if (m->room) {
            /* a room of about 5 x 4 m with the listener near the middle */
            float side = az >= 0 ? 100 : -100, mirror = 180 - az;
            float dirs[HRTF_REFLECTIONS] = {side, -side, mirror};
            float ms[HRTF_REFLECTIONS] = {6.5f, 12.5f, 9.0f}, g[HRTF_REFLECTIONS] = {0.38f, 0.26f, 0.30f};
            for (i = 0; i < HRTF_REFLECTIONS; i++) {
                float d = fmodf(dirs[i] + 720, 360);
                m->refl_dir[c][i] = (int)floorf(d / (360.0f / HRTF_ROOM_DIRS) + 0.5f) % HRTF_ROOM_DIRS;
                m->refl_delay[c][i] = (int)(ms[i] * size * rate / 1000);
                m->refl_gain[c][i] = g[i];
            }
        }
    }
    if (eq) {
        struct hrtf_ir *front = calloc(1, sizeof(*front));
        /* equalized, the front sound is about 3 dB under the plain mix: level with it, as Dolby is */
        for (c = 0; c < channels; c++) m->gain[c] *= 1.41f;
        hrtf_load(front, 0, rate, norm, cues, avg);
        hrtf_front_eq(m, front);
        free(front);
    }
    if (m->room) {
        for (i = 0; i < HRTF_ROOM_DIRS; i++) hrtf_load(&m->ir[HRTF_MAX_CHANNELS + i], i * 360.0f / HRTF_ROOM_DIRS, rate, norm, cues, avg);
        m->lp_k = 1 - expf(-2 * (float)M_PI * 5000 / rate);      /* walls take the highs off */
        for (i = 0; i < HRTF_FDN; i++) {
            m->fdn_len[i] = (int)((fdn_ms[i] + 0.1f * (i * 3 + 1)) * size * rate / 1000);
            m->fdn[i] = calloc(m->fdn_len[i], sizeof(float));
        }
        /* about 0.4 s x size to fall 60 dB: lines and reverb time scale together, so the gain per
           pass stays the same */
        m->fdn_g = powf(10, -3.0f * 0.040f / 0.40f);
        m->fdn_k = 1 - expf(-2 * (float)M_PI * 4000 / rate);
        m->tail_delay = (int)(0.025f * sqrtf(size) * rate);   /* after the direct sound, as Dolby's room starts */
    }
    return m;
}

static void hrtf_free(struct hrtf *m)
{
    int i;
    if (!m) return;
    for (i = 0; i < HRTF_FDN; i++) free(m->fdn[i]);
    free(m);
}

static void hrtf_convolve(struct hrtf *m, int src, float *out)
{
    const struct hrtf_ir *ir = &m->ir[src];
    const float *x = m->hist[src];
    float l = 0, r = 0;
    int k;
    for (k = 0; k < ir->taps; k++) {
        float v = x[(m->pos - k) & (HRTF_HIST - 1)];
        l += ir->h[0][k] * v;
        r += ir->h[1][k] * v;
    }
    out[0] += l;
    out[1] += r;
}

/* One frame of floats, in[channels], to out[2]. */
static void hrtf_frame(struct hrtf *m, const float *in, float *out)
{
    float room_in[HRTF_ROOM_DIRS] = {0}, mono = 0;
    int c, i;

    out[0] = out[1] = 0;
    m->pos = (m->pos + 1) & (HRTF_HIST - 1);
    for (c = 0; c < m->channels; c++) {
        float x = in[c] * m->gain[c];
        if (m->lfe[c]) { out[0] += x; out[1] += x; continue; }
        m->hist[c][m->pos] = x;
        hrtf_convolve(m, c, out);
        if (m->room) {
            for (i = 0; i < HRTF_REFLECTIONS; i++)
                room_in[m->refl_dir[c][i]] += m->room_gain * m->refl_gain[c][i] * m->hist[c][(m->pos - m->refl_delay[c][i]) & (HRTF_HIST - 1)];
            mono += m->hist[c][(m->pos - m->tail_delay) & (HRTF_HIST - 1)];
        }
    }
    if (!m->room) goto eq;
    for (i = 0; i < HRTF_ROOM_DIRS; i++) {
        int src = HRTF_MAX_CHANNELS + i;
        m->room_lp[i] += m->lp_k * (room_in[i] - m->room_lp[i]);
        m->hist[src][m->pos] = m->room_lp[i];
        hrtf_convolve(m, src, out);
    }
    {
        /* tail: Householder feedback matrix, damped, decorrelated left and right outputs */
        float y[HRTF_FDN], sum = 0, feed;
        for (i = 0; i < HRTF_FDN; i++) {
            y[i] = m->fdn[i][m->fdn_pos[i]];
            m->fdn_damp[i] += m->fdn_k * (y[i] - m->fdn_damp[i]);
            sum += m->fdn_damp[i];
        }
        for (i = 0; i < HRTF_FDN; i++) {
            feed = m->fdn_g * (m->fdn_damp[i] - 0.5f * sum) + 0.12f * mono;
            m->fdn[i][m->fdn_pos[i]] = feed;
            if (++m->fdn_pos[i] == m->fdn_len[i]) m->fdn_pos[i] = 0;
        }
        out[0] += 0.10f * m->tail_gain * (y[0] - y[2] + 0.5f * y[1]);
        out[1] += 0.10f * m->tail_gain * (y[1] - y[3] + 0.5f * y[2]);
    }
eq:
    if (m->eq) {
        int e, k;
        m->eq_pos = (m->eq_pos + 1) & 1023;
        for (e = 0; e < 2; e++) {
            float sum = 0;
            m->eq_hist[e][m->eq_pos] = out[e];
            for (k = 0; k < HRTF_EQ_TAPS; k++) sum += m->eq_h[k] * m->eq_hist[e][(m->eq_pos - k) & 1023];
            out[e] = sum;
        }
    }
}

#endif
