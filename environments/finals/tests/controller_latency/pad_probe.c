/* Controller latency probe, stage 1: the macOS side of the path, without Wine or the game.
 *
 * Copies what winebus.sys's SDL bus does inside winedevice.exe (dlls/winebus.sys/bus_sdl.c):
 * dlopen()s the runtime's libSDL2, sets the same hints, SDL_Init(GAMECONTROLLER | HAPTIC), and runs
 * `SDL_WaitEventTimeout(&event, 10)` on a second thread while the main thread runs a CFRunLoop,
 * as Wine's macOS loader does. Next to it, an IOHIDManager on its own thread receives the same
 * controllers' raw input reports with the kernel's arrival timestamp.
 *
 * Reports, per controller: the SDL driver that took it (HIDAPI / MFi / IOKit), the report rate and
 * its jitter, and for every button press or release the time from the report that carried it
 * reaching macOS to SDL handing the event to winebus's loop.
 *
 *   pad_probe [seconds] [path to libSDL2-2.0.0.dylib]
 *
 * Press buttons one at a time, a few dozen times. Ctrl-C (or the time limit) prints the summary.
 * Reads controllers only; nothing is written to them beyond what SDL itself sends (it switches
 * Switch-protocol pads into their full report mode, as it does inside Wine). */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEVS 8
#define MAX_INTERVALS 65536
#define MAX_PRESSES 4096
#define RING 256

static double ticks_to_ms;
static double now_ms(void) { return mach_absolute_time() * ticks_to_ms; }

/* ---- raw reports (IOHID thread) ---- */

/* Times of raw reports that changed something, newest last (a ring). */
struct ring { double t[RING]; int n; };
static void ring_add(struct ring *r, double t) { r->t[r->n++ % RING] = t; }

struct dev {
    IOHIDDeviceRef ref;
    char name[128], transport[32];
    long vid, pid;
    uint8_t buf[512];
    double last_t, last_change_t;
    float *idle, *busy; int n_idle, n_busy;          /* report intervals, untouched vs in use */
    uint8_t last_btn[8]; int last_btnlen;
    int last_stick[4], have;
    struct ring btn_changes, stick_changes;
    long reports, report_ids[256];
};
static struct dev devs[MAX_DEVS];
static int n_devs;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* Splits a report into its button bytes and stick values, so timer/IMU bytes and stick noise at
 * rest don't count as changes.
 * Switch full report 0x30/0x31: [0]=id [1]=timer [2]=battery [3..5]=buttons (ZL/ZR included,
 * which SDL reports as trigger axes), [6..8] left stick, [9..11] right stick (12-bit pairs).
 * Switch simple report 0x3F: [1..2] buttons, [3] hat, then 16-bit sticks.
 * Any other format: the first 8 bytes after the id count as both buttons and sticks. */
static int split_report(const uint8_t *r, CFIndex len, uint8_t *btn, int *stick, int *has_stick)
{
    *has_stick = 0;
    if (len >= 12 && (r[0] == 0x30 || r[0] == 0x31 || r[0] == 0x21))   /* 0x21 replies carry the same input bytes */
    {
        memcpy(btn, r + 3, 3);
        stick[0] = r[6] | ((r[7] & 0xf) << 8);  stick[1] = (r[7] >> 4) | (r[8] << 4);
        stick[2] = r[9] | ((r[10] & 0xf) << 8); stick[3] = (r[10] >> 4) | (r[11] << 4);
        *has_stick = 1;
        return 3;
    }
    if (len >= 12 && r[0] == 0x3f)
    {
        memcpy(btn, r + 1, 3);
        for (int i = 0; i < 4; i++) stick[i] = (r[4 + 2 * i] | (r[5 + 2 * i] << 8)) >> 4;   /* to ~12 bits */
        *has_stick = 1;
        return 3;
    }
    int n = len - 1 < 8 ? (int)len - 1 : 8;
    if (n < 0) n = 0;
    memcpy(btn, r + 1, n);
    return n;
}

#define STICK_NOISE 24   /* of 4096: smaller moves are jitter at rest, not stick movement */

static void report_cb(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id,
                      uint8_t *report, CFIndex len, uint64_t timestamp)
{
    struct dev *d = ctx;
    double t = timestamp * ticks_to_ms;
    /* Switch USB command replies (0x81, and 0x80 on some pads) carry no input. */
    if (len > 0 && (report[0] == 0x81 || report[0] == 0x80)) { pthread_mutex_lock(&lock); d->report_ids[report[0]]++; pthread_mutex_unlock(&lock); return; }
    uint8_t btn[8];
    int stick[4], has_stick;
    int n = split_report(report, len, btn, stick, &has_stick);
    pthread_mutex_lock(&lock);
    d->reports++;
    d->report_ids[report[0]]++;
    if (d->last_t > 0)
    {
        /* in use = something changed within the last 100 ms */
        if (t - d->last_change_t < 100) { if (d->n_busy < MAX_INTERVALS) d->busy[d->n_busy++] = t - d->last_t; }
        else if (d->n_idle < MAX_INTERVALS) d->idle[d->n_idle++] = t - d->last_t;
    }
    d->last_t = t;
    int btn_changed = n != d->last_btnlen || memcmp(btn, d->last_btn, n);
    int stick_changed = 0;
    if (has_stick)
        for (int i = 0; i < 4; i++)
            if (abs(stick[i] - d->last_stick[i]) > STICK_NOISE) stick_changed = 1;
    if (d->have)
    {
        if (btn_changed) ring_add(&d->btn_changes, t);
        if (stick_changed || (!has_stick && btn_changed)) ring_add(&d->stick_changes, t);
        if (btn_changed || stick_changed) d->last_change_t = t;
    }
    if (btn_changed || !d->have) { memcpy(d->last_btn, btn, n); d->last_btnlen = n; }
    if (stick_changed || !d->have) memcpy(d->last_stick, stick, sizeof(stick));
    d->have = 1;
    pthread_mutex_unlock(&lock);
    /* What macOS delivers, at most 5 times a second, so a stick that SDL misses still shows. */
    static double last_print;
    if (has_stick && stick_changed && t - last_print > 200)
    {
        last_print = t;
        printf("raw   report 0x%02x  L %4d,%4d  R %4d,%4d\n", report[0], stick[0], stick[1], stick[2], stick[3]);
    }
    else if (!has_stick && btn_changed && t - last_print > 200)
    {
        last_print = t;
        printf("raw   report 0x%02x (%ld bytes, layout not decoded):", report[0], (long)len);
        for (int i = 0; i < len && i < 16; i++) printf(" %02x", report[i]);
        printf("\n");
    }
}

static void prop_str(IOHIDDeviceRef ref, CFStringRef key, char *out, size_t size)
{
    CFTypeRef v = IOHIDDeviceGetProperty(ref, key);
    out[0] = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString(v, out, size, kCFStringEncodingUTF8);
}
static long prop_num(IOHIDDeviceRef ref, CFStringRef key)
{
    CFTypeRef v = IOHIDDeviceGetProperty(ref, key);
    long n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberLongType, &n);
    return n;
}

static void match_cb(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef ref)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n_devs; i++) if (devs[i].ref == ref) { pthread_mutex_unlock(&lock); return; }
    if (n_devs == MAX_DEVS) { pthread_mutex_unlock(&lock); return; }
    struct dev *d = &devs[n_devs++];
    memset(d, 0, sizeof(*d));
    d->ref = ref;
    d->idle = calloc(MAX_INTERVALS, sizeof(float));
    d->busy = calloc(MAX_INTERVALS, sizeof(float));
    prop_str(ref, CFSTR(kIOHIDProductKey), d->name, sizeof(d->name));
    prop_str(ref, CFSTR(kIOHIDTransportKey), d->transport, sizeof(d->transport));
    d->vid = prop_num(ref, CFSTR(kIOHIDVendorIDKey));
    d->pid = prop_num(ref, CFSTR(kIOHIDProductIDKey));
    pthread_mutex_unlock(&lock);
    IOHIDDeviceRegisterInputReportWithTimeStampCallback(ref, d->buf, sizeof(d->buf), report_cb, d);
    printf("raw: %s (%04lx:%04lx, %s)\n", d->name, d->vid, d->pid, d->transport);
}

static CFDictionaryRef usage_match(int page, int usage)
{
    CFMutableDictionaryRef m = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &page), u = CFNumberCreate(NULL, kCFNumberIntType, &usage);
    CFDictionarySetValue(m, CFSTR(kIOHIDDeviceUsagePageKey), p);
    CFDictionarySetValue(m, CFSTR(kIOHIDDeviceUsageKey), u);
    CFRelease(p); CFRelease(u);
    return m;
}

static void *hid_thread(void *arg)
{
    IOHIDManagerRef mgr = IOHIDManagerCreate(NULL, kIOHIDOptionsTypeNone);
    CFDictionaryRef m[3] = { usage_match(1, 4), usage_match(1, 5), usage_match(1, 8) };   /* joystick, gamepad, multi-axis */
    CFArrayRef arr = CFArrayCreate(NULL, (const void **)m, 3, &kCFTypeArrayCallBacks);
    IOHIDManagerSetDeviceMatchingMultiple(mgr, arr);
    IOHIDManagerRegisterDeviceMatchingCallback(mgr, match_cb, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOReturn r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) fprintf(stderr, "IOHIDManagerOpen: 0x%x (raw report times unavailable)\n", r);
    CFRunLoopRun();
    return NULL;
}

/* ---- SDL, as winebus does it ---- */

#define F(n) static __typeof__(n) *p##n
F(SDL_Init); F(SDL_SetHint); F(SDL_WaitEventTimeout); F(SDL_GetError); F(SDL_IsGameController);
F(SDL_GameControllerOpen); F(SDL_JoystickOpen); F(SDL_JoystickNameForIndex); F(SDL_JoystickGetDeviceGUID);
F(SDL_JoystickGetGUIDString); F(SDL_JoystickEventState); F(SDL_GameControllerEventState); F(SDL_GetVersion);
F(SDL_GameControllerGetJoystick); F(SDL_JoystickInstanceID);
#undef F

/* Report -> SDL delays, by kind of input. */
enum { K_BUTTON, K_TRIGGER, K_STICK, K_COUNT };
static const char *kind_name[K_COUNT] = { "buttons", "triggers (LT/RT, ZL/ZR)", "sticks" };
static double *delays[K_COUNT];
static int n_delays[K_COUNT], n_events[K_COUNT];
static volatile sig_atomic_t stop;

static const char *driver_of(SDL_JoystickGUID g)
{
    /* SDL2 marks the driver in byte 14 of the GUID (SDL_joystick.c / SDL_hidapijoystick.c). */
    switch (g.data[14]) {
    case 'h': return "HIDAPI";
    case 'm': return "MFi (GameController framework)";
    case 'v': return "virtual";
    case 'x': return "XInput";
    default:  return "IOKit";
    }
}

/* Records one SDL event: the delay from the newest raw report at or before it that changed the
 * same kind of input (buttons, or sticks), on any controller, within 100 ms. Returns it, or -1. */
static double on_input(int kind)
{
    double t = now_ms(), best = -1;
    pthread_mutex_lock(&lock);
    n_events[kind]++;
    for (int i = 0; i < n_devs; i++)
    {
        struct ring *r = kind == K_STICK ? &devs[i].stick_changes : &devs[i].btn_changes;
        int count = r->n < RING ? r->n : RING;
        for (int k = 0; k < count; k++)
        {
            double ct = r->t[(r->n - 1 - k) % RING];
            if (ct <= t && t - ct < 100 && ct > best) best = ct;
        }
    }
    double delay = best > 0 ? t - best : -1;
    if (delay >= 0 && n_delays[kind] < MAX_PRESSES * 16) delays[kind][n_delays[kind]++] = delay;
    pthread_mutex_unlock(&lock);
    return delay;
}

static void print_press(const char *what, int down, double delay)
{
    if (delay >= 0) printf("%-12s %-4s report->SDL %.3f ms\n", what, down ? "down" : "up", delay);
    else printf("%-12s %-4s (no raw report matched)\n", what, down ? "down" : "up");
}

static const char *button_name(int b)
{
    static const char *n[] = { "A", "B", "X", "Y", "Back", "Guide", "Start", "LStick", "RStick", "LB", "RB",
                               "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "Misc", "Paddle1", "Paddle2",
                               "Paddle3", "Paddle4", "Touchpad" };
    return b >= 0 && b < (int)(sizeof(n) / sizeof(*n)) ? n[b] : "button";
}

static void *sdl_thread(void *arg)
{
    SDL_Event e;
    while (!stop)
    {
        if (pSDL_WaitEventTimeout(&e, 10) == 0) continue;
        switch (e.type)
        {
        case SDL_JOYDEVICEADDED:
        {
            int idx = e.jdevice.which;
            SDL_JoystickGUID g = pSDL_JoystickGetDeviceGUID(idx);
            char gs[64];
            pSDL_JoystickGetGUIDString(g, gs, sizeof(gs));
            int gc = pSDL_IsGameController(idx);
            printf("SDL: \"%s\" via %s%s  guid %s\n", pSDL_JoystickNameForIndex(idx), driver_of(g),
                   gc ? ", game controller" : "", gs);
            if (gc) pSDL_GameControllerOpen(idx); else pSDL_JoystickOpen(idx);
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
        {
            int down = e.type == SDL_CONTROLLERBUTTONDOWN;
            print_press(button_name(e.cbutton.button), down, on_input(K_BUTTON));
            break;
        }
        case SDL_CONTROLLERAXISMOTION:
        {
            int axis = e.caxis.axis, v = e.caxis.value;
            if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
            {
                /* A trigger counts as pressed past half travel, released below a quarter
                 * (digital ZL/ZR jump straight between 0 and 32767). */
                static int held[2];
                int i = axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
                const char *name = i ? "RT" : "LT";
                if (!held[i] && v > 16384) { held[i] = 1; print_press(name, 1, on_input(K_TRIGGER)); }
                else if (held[i] && v < 8192) { held[i] = 0; print_press(name, 0, on_input(K_TRIGGER)); }
            }
            else
            {
                static int axes[4];
                static double last_print;
                double dl = on_input(K_STICK), t = now_ms();
                if (axis < 4) axes[axis] = v;
                if (t - last_print > 200)
                {
                    last_print = t;
                    printf("SDL   stick         L %6d,%6d  R %6d,%6d  (%d events, latest report->SDL %s)\n",
                           axes[0], axes[1], axes[2], axes[3], n_events[K_STICK],
                           dl < 0 ? "unmatched" : "matched");
                }
            }
            break;
        }
        case SDL_JOYAXISMOTION:
        {
            /* Every controller axis also arrives as a raw joystick axis; counted, to tell "SDL saw
             * nothing" from "SDL's controller mapping dropped it". */
            static int n, last_shown;
            if (++n - last_shown >= 200) { last_shown = n; printf("SDL   joystick axis events: %d\n", n); }
            break;
        }
        }
    }
    return NULL;
}

/* ---- summary ---- */

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static void summary(void)
{
    pthread_mutex_lock(&lock);
    printf("\n==== summary ====\n");
    for (int i = 0; i < n_devs; i++)
    {
        struct dev *d = &devs[i];
        if (!d->reports) { printf("%s (%s): no reports\n", d->name, d->transport); continue; }
        printf("%s (%04lx:%04lx, %s): %ld reports, ids", d->name, d->vid, d->pid, d->transport, d->reports);
        for (int k = 0; k < 256; k++) if (d->report_ids[k]) printf(" 0x%02x×%ld", k, d->report_ids[k]);
        printf("\n");
        for (int busy = 0; busy < 2; busy++)
        {
            int n = busy ? d->n_busy : d->n_idle;
            const char *label = busy ? "in use   " : "untouched";
            if (n <= 10) { printf("  report interval %s: too few reports\n", label); continue; }
            float *v = malloc(n * sizeof(float));
            memcpy(v, busy ? d->busy : d->idle, n * sizeof(float));
            qsort(v, n, sizeof(float), cmpf);
            float med = v[n / 2];
            printf("  report interval %s: median %.2f ms (%.0f Hz), p5 %.2f, p95 %.2f, p99 %.2f, max %.2f ms  (%d)\n",
                   label, med, med > 0 ? 1000 / med : 0, v[n * 5 / 100], v[n * 95 / 100], v[n * 99 / 100], v[n - 1], n);
            free(v);
        }
    }
    printf("report reaching macOS -> SDL event in winebus's loop:\n");
    for (int k = 0; k < K_COUNT; k++)
    {
        int m = n_delays[k];
        if (!m) { printf("  %-24s no events matched (%d SDL events)\n", kind_name[k], n_events[k]); continue; }
        double sum = 0;
        qsort(delays[k], m, sizeof(double), cmpd);
        for (int i = 0; i < m; i++) sum += delays[k][i];
        printf("  %-24s mean %.3f, median %.3f, p95 %.3f, max %.3f ms  (%d of %d events matched)\n", kind_name[k],
               sum / m, delays[k][m / 2], delays[k][m * 95 / 100], delays[k][m - 1], m, n_events[k]);
    }
    pthread_mutex_unlock(&lock);
}

static void on_signal(int s) { stop = 1; }
static void *timer_thread(void *arg)
{
    int secs = *(int *)arg;
    for (int i = 0; i < secs * 10 && !stop; i++) usleep(100000);
    stop = 1;
    return NULL;
}
static void *stopper(void *arg)
{
    while (!stop) usleep(50000);
    usleep(50000);
    summary();
    exit(0);
}

int main(int argc, char **argv)
{
    static int secs;
    secs = argc > 1 ? atoi(argv[1]) : 60;
    const char *lib = argc > 2 ? argv[2] :
        "/Users/rasmus/SteamProfiles/main/data/compatibilitytools.d/notproton-steamdriver/runtime/lib/libSDL2-2.0.0.dylib";
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    ticks_to_ms = (double)tb.numer / tb.denom / 1e6;
    for (int k = 0; k < K_COUNT; k++) delays[k] = calloc(MAX_PRESSES * 16, sizeof(double));

    void *h = dlopen(lib, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 1; }
#define L(n) if (!(p##n = dlsym(h, #n))) { fprintf(stderr, "missing %s\n", #n); return 1; }
    L(SDL_Init); L(SDL_SetHint); L(SDL_WaitEventTimeout); L(SDL_GetError); L(SDL_IsGameController);
    L(SDL_GameControllerOpen); L(SDL_JoystickOpen); L(SDL_JoystickNameForIndex); L(SDL_JoystickGetDeviceGUID);
    L(SDL_JoystickGetGUIDString); L(SDL_JoystickEventState); L(SDL_GameControllerEventState); L(SDL_GetVersion);
    L(SDL_GameControllerGetJoystick); L(SDL_JoystickInstanceID);
#undef L
    SDL_version v;
    pSDL_GetVersion(&v);
    printf("SDL %d.%d.%d from %s\n", v.major, v.minor, v.patch, lib);

    /* winebus.sys bus_sdl.c, sdl_driver_init (CW HACK 19629 hints, then init). */
    pSDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    pSDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    if (pSDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0) { fprintf(stderr, "SDL_Init: %s\n", pSDL_GetError()); return 1; }
    pSDL_JoystickEventState(SDL_ENABLE);
    pSDL_GameControllerEventState(SDL_ENABLE);

    signal(SIGINT, on_signal);
    pthread_t t1, t2, t3, t4;
    pthread_create(&t1, NULL, hid_thread, NULL);
    pthread_create(&t2, NULL, sdl_thread, NULL);
    pthread_create(&t3, NULL, timer_thread, &secs);
    pthread_create(&t4, NULL, stopper, NULL);
    printf("press buttons one at a time; %d s or Ctrl-C for the summary\n", secs);
    CFRunLoopRun();   /* the main thread runs a CFRunLoop, as in every Wine process on macOS */
    return 0;
}
