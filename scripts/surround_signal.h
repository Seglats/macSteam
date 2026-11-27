/* The headphone surround test, shared by scripts/surround_test.c (played through Wine) and
 * scripts/surround_render.py (written to files, rendered by the Wine patch's own model), so both
 * play exactly the same thing. Plain C, no Windows or Mac headers.
 *
 * One ball bounce (scripts/surround/ball-bounce.wav, cut from a recording: README.md there)
 * repeated at different spots around the listener, in five patterns with a short pause between
 * them. Each hit is panned to its direction between the two nearest 7.1 speakers with equal power,
 * as a game's audio engine pans a sound.
 *
 * 7.1 channel order (Windows, mask 0x63F): FL FR FC LFE BL BR SL SR. */
#ifndef SURROUND_SIGNAL_H
#define SURROUND_SIGNAL_H

#include <math.h>
#include <string.h>

#define SIG_CHANNELS 8
#define SIG_PASSES 5
#define SIG_MAX_HITS 128
#define SIG_PAUSE 0.8   /* seconds between patterns */

static const char *const sig_pass_names[SIG_PASSES] = {
    "a slow lap clockwise", "two fast laps counterclockwise", "jumps clockwise, three hits a spot",
    "front and back", "right and left",
};

/* Speakers clockwise from the front, degrees (right positive), for panning. */
static const struct { float az; int channel; } sig_ring[] = {
    {0, 2}, {30, 1}, {90, 7}, {150, 5}, {210, 4}, {270, 6}, {330, 0},
};

struct sig_hit { double t; float az; int pass; };

struct sig
{
    unsigned int rate;
    const float *clip;            /* the bounce, mono, at rate */
    unsigned long long clip_frames, total;
    int hits, next;               /* next: first hit that may still sound (frames go forward) */
    struct sig_hit hit[SIG_MAX_HITS];
};

static void sig_add(struct sig *s, double t, float az, int pass)
{
    if (s->hits < SIG_MAX_HITS) { s->hit[s->hits].t = t; s->hit[s->hits].az = fmodf(az + 720.0f, 360.0f); s->hit[s->hits].pass = pass; s->hits++; }
}

static void sig_init(struct sig *s, const float *clip, unsigned long long clip_frames, unsigned int rate)
{
    double t = 0.3;
    int i, j;
    memset(s, 0, sizeof(*s));
    s->rate = rate; s->clip = clip; s->clip_frames = clip_frames;
    for (i = 0; i < 16; i++, t += 0.40) sig_add(s, t, 22.5f * i, 0);            /* slow lap clockwise */
    t += SIG_PAUSE;
    for (i = 0; i < 24; i++, t += 0.20) sig_add(s, t, -30.0f * i, 1);           /* two fast laps counterclockwise */
    t += SIG_PAUSE;
    for (i = 0; i < 8; i++, t += 0.45)                                           /* jumps: 8 spots, 3 hits each */
        for (j = 0; j < 3; j++, t += 0.25) sig_add(s, t, 45.0f * i, 2);
    t += SIG_PAUSE;
    for (i = 0; i < 12; i++, t += 0.50) sig_add(s, t, i % 2 ? 180.0f : 0.0f, 3); /* front, back */
    t += SIG_PAUSE;
    for (i = 0; i < 12; i++, t += 0.50) sig_add(s, t, i % 2 ? 270.0f : 90.0f, 4);/* right, left */
    s->total = (unsigned long long)((t + 0.5) * rate);
}

/* x panned to az (degrees clockwise from the front) between the two nearest 7.1 speakers, with
 * equal power, added to out[SIG_CHANNELS]. */
static void sig_pan(float x, float az, float *out)
{
    int i;
    az = fmodf(az, 360.0f);
    if (az < 0) az += 360.0f;
    for (i = 0; i < 7; i++) {
        float a = sig_ring[i].az, b = i == 6 ? 360 : sig_ring[i + 1].az, frac;
        if (az >= a && az < b) {
            frac = (az - a) / (b - a);
            out[sig_ring[i].channel] += x * cosf(frac * 3.14159265f / 2);
            out[sig_ring[(i + 1) % 7].channel] += x * sinf(frac * 3.14159265f / 2);
            return;
        }
    }
}

/* The hit heard at frame n (the last one started), or -1 before the first. */
static int sig_hit_at(const struct sig *s, unsigned long long n)
{
    int i, found = -1;
    for (i = 0; i < s->hits; i++) if ((unsigned long long)(s->hit[i].t * s->rate) <= n) found = i;
    return found;
}

/* Frame n into out[SIG_CHANNELS], frames in order; returns 0 past the end. */
static int sig_frame(struct sig *s, unsigned long long n, float *out)
{
    int i;
    memset(out, 0, SIG_CHANNELS * sizeof(float));
    if (n >= s->total) return 0;
    while (s->next < s->hits && (unsigned long long)(s->hit[s->next].t * s->rate) + s->clip_frames <= n) s->next++;
    for (i = s->next; i < s->hits; i++) {
        unsigned long long start = (unsigned long long)(s->hit[i].t * s->rate);
        if (start > n) break;
        if (n - start < s->clip_frames) sig_pan(s->clip[n - start], s->hit[i].az, out);
    }
    return 1;
}

#endif
