/* Headphone surround model with every knob, for fitting (fit2.py). Plain C.
 *
 * render2 HEAD.head OUT.f32 [ball] [name=value ...]
 *   input: stress-7.1.f32 (8 channels, 48 kHz), or with "ball" the ball test (surround_signal.h)
 *   HEAD.head: heads.py table (level per bin and onset per ear, azimuth x elevation grid)
 *
 * Per speaker: the head's response for its direction (interpolated between grid points), reshaped
 * (cues, smoothing, low-frequency crossover, front EQ, shelves) and rebuilt as a minimum-phase
 * filter plus each ear's onset delay. Then group gains and delays, early reflections from 12 room
 * directions, and a 4-line reverb tail. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "surround_signal.h"

#define NF 512
#define NB (NF / 2 + 1)
#define NAZ 72
#define NEL 10
#define TAPS 384      /* the most a filter may have */
#define SRC (8 + 12)
#ifndef SPK_TAPS
#define SPK_TAPS 256
#endif
#define HIST 32768
#define R 48000.0f

static float head_db[NEL][NAZ][2][NB], head_dl[NEL][NAZ][2];

/* knobs */
static struct { const char *name; float v; } K[] = {
    {"itd", 1}, {"ild", 1}, {"ild_lo", 1}, {"ild_hi", 1}, {"contrast", 1}, {"smooth", 0}, {"xover", 0},
    {"eq", 1}, {"lo_g", 0}, {"lo_f", 200}, {"hi_g", 0}, {"hi_f", 6000},
    {"front", 30}, {"side", 90}, {"back", 150}, {"el_front", 0}, {"el_side", 0}, {"el_back", 0},
    {"g_side", 0}, {"g_back", 0}, {"d_side", 0}, {"d_back", 0},
    {"early", 0}, {"tail", 0}, {"room_lp", 5000}, {"tail_lp", 4000}, {"rt", 0.4f}, {"tail_ms", 25},
    {"gain", 0.5f},
};
static float knob(const char *n)
{
    for (unsigned i = 0; i < sizeof(K) / sizeof(K[0]); i++) if (!strcmp(K[i].name, n)) return K[i].v;
    fprintf(stderr, "no knob %s\n", n); exit(1);
}
/* 0 when there is no such knob */
__attribute__((unused)) static int set_knob(const char *n, float v)
{
    for (unsigned i = 0; i < sizeof(K) / sizeof(K[0]); i++) if (!strcmp(K[i].name, n)) { K[i].v = v; return 1; }
    return 0;
}

static void fft(float *re, float *im, int n, int inv)
{
    int i, j, len;
    for (i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (len = 2; len <= n; len <<= 1) {
        float a = 2 * (float)M_PI / len * (inv ? 1 : -1);
        for (i = 0; i < n; i += len)
            for (j = 0; j < len / 2; j++) {
                float wr = cosf(a * j), wi = sinf(a * j);
                float vr = re[i + j + len / 2] * wr - im[i + j + len / 2] * wi, vi = re[i + j + len / 2] * wi + im[i + j + len / 2] * wr;
                re[i + j + len / 2] = re[i + j] - vr; im[i + j + len / 2] = im[i + j] - vi;
                re[i + j] += vr; im[i + j] += vi;
            }
    }
    if (inv) for (i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}

static void minphase(const float *db, float *h)
{
    float re[NF], im[NF];
    int k;
    for (k = 0; k < NF; k++) { re[k] = db[k <= NF / 2 ? k : NF - k] * (float)M_LN10 / 20; im[k] = 0; }
    fft(re, im, NF, 1);
    for (k = 1; k < NF / 2; k++) { re[k] *= 2; im[k] *= 2; }
    for (k = NF / 2 + 1; k < NF; k++) re[k] = im[k] = 0;
    fft(re, im, NF, 0);
    for (k = 0; k < NF; k++) { float m = expf(re[k]); re[k] = m * cosf(im[k]); im[k] = m * sinf(im[k]); }
    fft(re, im, NF, 1);
    for (k = 0; k < NF; k++) h[k] = re[k];
}

/* the head's level and onsets for a direction, bilinear between grid points */
static void lookup(float az, float el, float db[2][NB], float dl[2])
{
    float fa, fe;
    int a0, a1, e0, e1, e, k;
    az = fmodf(az + 720, 360);
    fa = az / 5; a0 = (int)floorf(fa); fa -= a0; a0 %= NAZ; a1 = (a0 + 1) % NAZ;
    fe = (el + 30) / 10; if (fe < 0) fe = 0; if (fe > NEL - 1) fe = NEL - 1;
    e0 = (int)floorf(fe); if (e0 > NEL - 2) e0 = NEL - 2; fe -= e0; e1 = e0 + 1;
    for (e = 0; e < 2; e++) {
        for (k = 0; k < NB; k++)
            db[e][k] = (1 - fe) * ((1 - fa) * head_db[e0][a0][e][k] + fa * head_db[e0][a1][e][k])
                     + fe * ((1 - fa) * head_db[e1][a0][e][k] + fa * head_db[e1][a1][e][k]);
        dl[e] = (1 - fe) * ((1 - fa) * head_dl[e0][a0][e] + fa * head_dl[e0][a1][e])
              + fe * ((1 - fa) * head_dl[e1][a0][e] + fa * head_dl[e1][a1][e]);
    }
}

static float avg_db[NB], eq_db[NB];

static float shelf_db(float f, float fc, float g)    /* first-order shelf, g dB below fc (low) */
{
    float x = (f / fc) * (f / fc), lin = powf(10, g / 20);
    return 10 * log10f((lin * lin + x) / (1 + x));
}

struct filt { int taps; float h[2][TAPS + 64]; };

/* the response for a direction with every reshaping knob, as two FIRs with their onset delays */
static void make(float az, float el, struct filt *out, int with_eq, int n)
{
    static float db[2][NB], tmp[NB], h[NF];
    float dl[2], first, f, o, ild, mono, mono_new;
    int e, k, j;
    lookup(az, el, db, dl);
    if (knob("smooth") > 0)
        for (e = 0; e < 2; e++) {
            for (k = 1; k < NB; k++) {
                float lo = k * powf(2, -knob("smooth") / 2), hi = k * powf(2, knob("smooth") / 2), s = 0; int c = 0;
                for (j = (int)ceilf(lo); j <= hi && j < NB; j++) { s += db[e][j]; c++; }
                tmp[k] = c ? s / c : db[e][k];
            }
            for (k = 1; k < NB; k++) db[e][k] = tmp[k];
        }
    for (k = 0; k < NB; k++) {
        f = k * R / NF;
        mono = (db[0][k] + db[1][k]) / 2;
        mono_new = avg_db[k] + knob("contrast") * (mono - avg_db[k]);
        o = f > 1 ? log2f(f / 2000) : -10;
        if (o < 0) ild = o <= -2 ? knob("ild_lo") : knob("ild") + (knob("ild_lo") - knob("ild")) * (-o / 2);
        else ild = o >= 2 ? knob("ild_hi") : knob("ild") + (knob("ild_hi") - knob("ild")) * (o / 2);
        for (e = 0; e < 2; e++) db[e][k] = mono_new + ild * (db[e][k] - mono);
    }
    if (knob("xover") > 20) {
        /* below the crossover: both ears get the level at the crossover, no level difference */
        int kx = (int)(knob("xover") * NF / R);
        if (kx >= NB) kx = NB - 1;
        float m = (db[0][kx] + db[1][kx]) / 2;
        for (k = 0; k < kx; k++) for (e = 0; e < 2; e++) db[e][k] = m;
    }
    for (k = 0; k < NB; k++) {
        f = k * R / NF;
        float add = (with_eq ? eq_db[k] : 0) + shelf_db(f > 1 ? f : 1, knob("lo_f"), knob("lo_g"))
                  + (knob("hi_g") - shelf_db(f > 1 ? f : 1, knob("hi_f"), knob("hi_g")));
        for (e = 0; e < 2; e++) db[e][k] += add;
    }
    first = dl[0] < dl[1] ? dl[0] : dl[1];
    out->taps = n + 48;
    for (e = 0; e < 2; e++) {
        float d = knob("itd") * (dl[e] - first);
        minphase(db[e], h);
        /* place the minimum-phase response at its onset delay (fractional: windowed sinc) */
        for (k = 0; k < out->taps; k++) {
            float s = 0;
            for (j = 0; j < n; j++) {
                float x = k - j - d, w;
                if (fabsf(x) >= 8) continue;
                w = 0.5f + 0.5f * cosf((float)M_PI * x / 8);
                s += h[j] * (x == 0 ? 1 : sinf((float)M_PI * x) / ((float)M_PI * x)) * w;
            }
            out->h[e][k] = s;
        }
        /* fade the last 64 taps */
        for (k = n - 16; k < out->taps; k++) out->h[e][k] *= 0.5f + 0.5f * cosf((float)M_PI * (k - n + 16) / (out->taps - n + 16));
    }
}

static struct filt F[SRC];
static float hist[SRC][2 * HIST];   /* each sample written twice: reads never wrap */
static unsigned pos;
static int delay_s[8], refl_dir[8][3], refl_delay[8][3];
static float gain_c[8], refl_gain[8][3], room_lp[12], lp_k;
static float *fdn[4], fdn_damp[4], fdn_g, fdn_k;
static int fdn_len[4], fdn_pos[4], tail_delay;
static float k_early, k_tail;

static inline void put(int s, float x) { hist[s][pos] = x; hist[s][pos + HIST] = x; }
static inline float at(int s, int back) { return hist[s][pos + HIST - back]; }
static void conv_at(int s, int delay, float *out)
{
    const float *x = hist[s] + pos + HIST - delay;   /* x[-k]: k samples earlier */
    const float *h0 = F[s].h[0], *h1 = F[s].h[1];
    float l = 0, r = 0;
    for (int k = 0; k < F[s].taps; k++) { float v = x[-k]; l += h0[k] * v; r += h1[k] * v; }
    out[0] += l; out[1] += r;
}
static void conv(int s, float *out) { conv_at(s, 0, out); }

static void frame(const float *in, float *out)
{
    float room_in[12] = {0}, mono = 0;
    int c, i;
    out[0] = out[1] = 0;
    pos = (pos + 1) & (HIST - 1);
    for (c = 0; c < 8; c++) {
        float x = in[c] * gain_c[c];
        if (c == 3) { out[0] += x; out[1] += x; continue; }
        put(c, x);
        conv_at(c, delay_s[c], out);                      /* group delay: read the history late */
        for (i = 0; i < 3; i++) room_in[refl_dir[c][i]] += refl_gain[c][i] * at(c, delay_s[c] + refl_delay[c][i]);
        mono += at(c, delay_s[c] + tail_delay);
    }
    if (k_early > 0)
        for (i = 0; i < 12; i++) {
            room_lp[i] += lp_k * (room_in[i] - room_lp[i]);
            put(8 + i, room_lp[i]);
            conv(8 + i, out);
        }
    if (k_tail > 0) {
        float y[4], sum = 0;
        for (i = 0; i < 4; i++) { y[i] = fdn[i][fdn_pos[i]]; fdn_damp[i] += fdn_k * (y[i] - fdn_damp[i]); sum += fdn_damp[i]; }
        for (i = 0; i < 4; i++) { fdn[i][fdn_pos[i]] = fdn_g * (fdn_damp[i] - 0.5f * sum) + 0.12f * mono; if (++fdn_pos[i] == fdn_len[i]) fdn_pos[i] = 0; }
        out[0] += 0.10f * k_tail * (y[0] - y[2] + 0.5f * y[1]);
        out[1] += 0.10f * k_tail * (y[1] - y[3] + 0.5f * y[2]);
    }
}

/* The head table (heads.py) and its average level over horizontal-ish directions (for contrast). */
static int load_head(const char *path)
{
    int c, k, a, e;
    FILE *hf = fopen(path, "rb");
    if (!hf) return 0;
    if (fread(head_db, sizeof(head_db), 1, hf) != 1 || fread(head_dl, sizeof(head_dl), 1, hf) != 1) { fclose(hf); return 0; }
    fclose(hf);
    memset(avg_db, 0, sizeof(avg_db));
    /* average level over the horizontal-ish directions (for contrast) */
    for (e = 0; e < NEL; e++) {
        float w = cosf((e * 10 - 30) * (float)M_PI / 180);
        for (a = 0; a < NAZ; a++) for (c = 0; c < 2; c++) for (k = 0; k < NB; k++) avg_db[k] += w * head_db[e][a][c][k];
    }
    {
        float wsum = 0;
        for (e = 0; e < NEL; e++) wsum += cosf((e * 10 - 30) * (float)M_PI / 180);
        for (k = 0; k < NB; k++) avg_db[k] /= wsum * NAZ * 2;
    }
    return 1;
}

/* Everything from the knobs: front EQ, the speakers' filters, gains and delays, early reflections and
 * the reverb tail. Call again after changing knobs (macSteam's live preview does). */
static void configure(void)
{
    int c, i, k;
    /* front EQ: the inverse of the reshaped front level (both ears), a third of an octave, +-12 dB */
    {
        struct filt tmpf;
        float db[2][NB], dl[2], fr[NB];
        lookup(0, knob("el_front"), db, dl);
        for (k = 0; k < NB; k++) { float m = (db[0][k] + db[1][k]) / 2; fr[k] = avg_db[k] + knob("contrast") * (m - avg_db[k]); }
        for (k = 0; k < NB; k++) {
            float f = k * R / NF, s = 0, g; int cnt = 0, j;
            for (j = 0; j < NB; j++) { float fj = j * R / NF; if (fj >= f / 1.1225f && fj <= f * 1.1225f) { s += powf(10, fr[j] / 10); cnt++; } }
            g = -10 * log10f(cnt ? s / cnt : powf(10, fr[k] / 10));
            if (g > 12) g = 12; if (g < -12) g = -12;
            if (f > 16000) g = 0;
            eq_db[k] = g * knob("eq");
        }
        (void)tmpf;
    }
    /* speakers: FL FR FC LFE BL BR SL SR */
    {
        float az[8] = {-knob("front"), knob("front"), 0, 0, -knob("back"), knob("back"), -knob("side"), knob("side")};
        float el[8] = {knob("el_front"), knob("el_front"), knob("el_front"), 0, knob("el_back"), knob("el_back"), knob("el_side"), knob("el_side")};
        float g[8] = {0, 0, 0, 0, knob("g_back"), knob("g_back"), knob("g_side"), knob("g_side")};
        float d[8] = {0, 0, 0, 0, knob("d_back"), knob("d_back"), knob("d_side"), knob("d_side")};
        for (c = 0; c < 8; c++) {
            gain_c[c] = c == 3 ? 0.25f : knob("gain") * powf(10, g[c] / 20);
            delay_s[c] = (int)(d[c] * R / 1000);
            if (c == 3) continue;
            make(az[c], el[c], &F[c], 1, SPK_TAPS);
            float side = az[c] >= 0 ? 100 : -100, dirs[3] = {side, -side, 180 - az[c]}, ms[3] = {6.5f, 12.5f, 9.0f}, rg[3] = {0.38f, 0.26f, 0.30f};
            for (i = 0; i < 3; i++) {
                refl_dir[c][i] = (int)floorf(fmodf(dirs[i] + 720, 360) / 30 + 0.5f) % 12;
                refl_delay[c][i] = (int)(ms[i] * R / 1000);
                refl_gain[c][i] = knob("early") * rg[i];
            }
        }
    }
    if (knob("early") > 0) for (i = 0; i < 12; i++) make(i * 30.0f, 0, &F[8 + i], 1, 128);
    lp_k = 1 - expf(-2 * (float)M_PI * knob("room_lp") / R);
    {
        static const int ms[4] = {29, 37, 41, 47};
        for (i = 0; i < 4; i++) if (!fdn[i]) { fdn_len[i] = (int)((ms[i] + 0.1f * (i * 3 + 1)) * R / 1000); fdn[i] = calloc(fdn_len[i], sizeof(float)); }
        fdn_g = powf(10, -3.0f * 0.040f / knob("rt"));
        fdn_k = 1 - expf(-2 * (float)M_PI * knob("tail_lp") / R);
        tail_delay = (int)(knob("tail_ms") / 1000 * R);
    }

    k_early = knob("early"); k_tail = knob("tail");
}

/* The driver's 12 speaker positions (C, FLC, FRC, FL, FR, SL, SR, S5L, S5R, BL, BR, BC): direction,
 * elevation and group gain (dB) from the knobs. 5.1's surrounds (S5) sit at 110 degrees. */
static void positions(float az[12], float el[12], float g[12])
{
    float fr = knob("front"), sd = knob("side"), bk = knob("back"), s5 = 110;
    float el_s5 = (knob("el_side") + knob("el_back")) / 2, g_s5 = (knob("g_side") + knob("g_back")) / 2;
    float a[12] = {0, -fr / 2, fr / 2, -fr, fr, -sd, sd, -s5, s5, -bk, bk, 180};
    float e[12] = {knob("el_front"), knob("el_front"), knob("el_front"), knob("el_front"), knob("el_front"),
                   knob("el_side"), knob("el_side"), el_s5, el_s5, knob("el_back"), knob("el_back"), knob("el_back")};
    float gg[12] = {0, 0, 0, 0, 0, knob("g_side"), knob("g_side"), g_s5, g_s5, knob("g_back"), knob("g_back"), knob("g_back")};
    memcpy(az, a, sizeof(a)); memcpy(el, e, sizeof(e)); memcpy(g, gg, sizeof(gg));
}

/* "bake": instead of rendering, write OUT as a C header with every speaker position's finished
 * filters at 48 kHz (what wine-coreaudio-headphone-surround-fitted.patch plays), the gain each
 * position's group gets and the reverb tail's settings. Early reflections are left out: at the
 * fitted level they change the score by under 1 % (docs/SURROUND.md). */
static int write_bake(const char *head, const char *path, int argc, char **argv)
{
    static const char *names[] = {"C", "FLC", "FRC", "FL", "FR", "SL", "SR", "S5L", "S5R", "BL", "BR", "BC"};
    float az[12], el[12], g[12];
    static struct filt f;
    const char *base = strrchr(head, '/');
    FILE *o = fopen(path, "w");
    int d, e, k, i;
    if (!o) { perror(path); return 1; }
    positions(az, el, g);
    fprintf(o, "/* Headphone surround's speaker filters: generated by scripts/surround_fit/render2 bake, do not edit.\n"
               " * Head: %s (MIT Media Lab KEMAR, Bill Gardner and Keith Martin, 1994: free to use with credit).\n"
               /* the SOFA file: no usage restrictions, citing Gardner and Martin (1995) requested
                  (docs/SURROUND.md). Kept as baked into the fitted patch, so a re-bake stays identical. */
               " * Settings fitted to Dolby Atmos for Headphones on a 5-minute stress test (docs/SURROUND.md):\n *  ",
            base ? base + 1 : head);
    for (i = 3; i < argc; i++) if (strcmp(argv[i], "bake")) fprintf(o, " %s=%s", argv[i], argv[i] + strlen(argv[i]) + 1);
    fprintf(o, "\n * Each position: a minimum-phase response per ear placed at its onset, 48 kHz. */\n\n");
    fprintf(o, "#define SURROUND_IR_RATE 48000\n#define SURROUND_IR_TAPS %d\n", SPK_TAPS + 48);
    fprintf(o, "enum surround_pos { SURROUND_C, SURROUND_FLC, SURROUND_FRC, SURROUND_FL, SURROUND_FR, SURROUND_SL, SURROUND_SR,\n"
               "                    SURROUND_S5L, SURROUND_S5R, SURROUND_BL, SURROUND_BR, SURROUND_BC, SURROUND_POSITIONS };\n");
    fprintf(o, "/* the speaker's level before its filter (the fitted gain of its group) */\n");
    fprintf(o, "static const float surround_gain[SURROUND_POSITIONS] = {");
    for (d = 0; d < 12; d++) fprintf(o, "%s%.6g", d ? ", " : "", knob("gain") * powf(10, g[d] / 20));
    fprintf(o, "};\n#define SURROUND_LFE_GAIN 0.25f\n");
    fprintf(o, "/* reverb tail: level, time to fall 60 dB (s), damping (Hz), start after the direct sound (ms) */\n");
    fprintf(o, "#define SURROUND_TAIL %.6gf\n#define SURROUND_TAIL_RT %.6gf\n#define SURROUND_TAIL_LP %.6gf\n#define SURROUND_TAIL_MS %.6gf\n",
            knob("tail"), knob("rt"), knob("tail_lp"), knob("tail_ms"));
    fprintf(o, "static const float surround_ir[SURROUND_POSITIONS][2][SURROUND_IR_TAPS] = {\n");
    for (d = 0; d < 12; d++) {
        make(az[d], el[d], &f, 1, SPK_TAPS);
        fprintf(o, "    /* %s: %.1f degrees, elevation %.1f */\n    {", names[d], az[d], el[d]);
        for (e = 0; e < 2; e++) {
            fprintf(o, "{");
            for (k = 0; k < f.taps; k++) fprintf(o, "%s%s%.6g", k ? "," : "", k && k % 12 == 0 ? "\n      " : "", f.h[e][k]);
            fprintf(o, "}%s", e ? "" : ",\n     ");
        }
        fprintf(o, "},\n");
    }
    fprintf(o, "};\n");
    fclose(o);
    return 0;
}

int main(int argc, char **argv)
{
    int ball = 0, bake = 0, i;
    if (argc < 3) { fprintf(stderr, "usage: render2 HEAD OUT [ball|bake] [knob=value ...]\n"); return 1; }
    for (i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!strcmp(argv[i], "ball")) { ball = 1; continue; }
        if (!strcmp(argv[i], "bake")) { bake = 1; continue; }
        if (!eq) { fprintf(stderr, "bad arg %s\n", argv[i]); return 1; }
        *eq = 0;
        unsigned j;
        for (j = 0; j < sizeof(K) / sizeof(K[0]); j++) if (!strcmp(K[j].name, argv[i])) { K[j].v = atof(eq + 1); break; }
        if (j == sizeof(K) / sizeof(K[0])) { fprintf(stderr, "no knob %s\n", argv[i]); return 1; }
    }
    if (!load_head(argv[1])) { fprintf(stderr, "bad head\n"); return 1; }
    configure();
    if (bake) return write_bake(argv[1], argv[2], argc, argv);
    FILE *o = fopen(argv[2], "wb");
    float in[8], out[2];
    static float buf[8192];
    int nb = 0;
    if (ball) {
        static float clip[48000];
        static struct sig s;
        FILE *cf = fopen("clip.f32", "rb");
        size_t n = fread(clip, 4, 48000, cf);
        fclose(cf);
        sig_init(&s, clip, n, 48000);
        for (unsigned long long t = 0; sig_frame(&s, t, in); t++) {
            frame(in, out); buf[nb++] = out[0]; buf[nb++] = out[1];
            if (nb == 8192) { fwrite(buf, 4, nb, o); nb = 0; }
        }
    } else {
        FILE *f = fopen("stress-7.1.f32", "rb");
        while (fread(in, 4, 8, f) == 8) {
            frame(in, out); buf[nb++] = out[0]; buf[nb++] = out[1];
            if (nb == 8192) { fwrite(buf, 4, nb, o); nb = 0; }
        }
        fclose(f);
    }
    fwrite(buf, 4, nb, o);
    fclose(o);
    return 0;
}
