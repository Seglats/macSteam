/* event_flood: post stationary mouse-moved events, loading the frontmost app's Cocoa main
 * thread (for THE FINALS under Wine: winemac's main thread runs postEventObject for every
 * event — the C06 stall suspect). Events are posted at the *current* cursor position (re-read
 * 50 times a second), so the pointer never moves and the game sees zero-length moves.
 *
 * Rate is events/s (default 500). Verify delivery with WINEMAC_INPUT_STATS' mac_moves counter.
 * Posting needs this binary trusted in System Settings -> Privacy & Security -> Accessibility;
 * untrusted posts are dropped silently (mac_moves won't move). Ctrl-C stops.
 *
 * clang -O2 event_flood.c -framework CoreGraphics -o event_flood && ./event_flood [rate]
 */
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

int main(int argc, char **argv)
{
    int rate = argc > 1 ? atoi(argv[1]) : 500, wig = argc > 2 ? atoi(argv[2]) : 0, sent = 0;
    CGPoint pos = CGPointMake(0, 0);
    CGEventSourceRef src;

    if (argc > 1 && !strcmp(argv[1], "--ask"))
    {
        CFStringRef key = kAXTrustedCheckOptionPrompt;
        CFBooleanRef value = kCFBooleanTrue;
        CFDictionaryRef opts = CFDictionaryCreate(NULL, (const void **)&key, (const void **)&value,
                                                  1, NULL, NULL);
        int trusted = AXIsProcessTrustedWithOptions(opts);
        CFRelease(opts);
        printf("accessibility trusted: %d\n", trusted);
        return 0;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    src = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
    printf("posting %d mouse-moves/s (wiggle %d px); Ctrl-C stops\n", rate, wig);
    fflush(stdout);
    while (!stop)
    {
        CGEventRef e;
        if (sent % 10 == 0)   /* follow the real cursor ~50 times a second */
        {
            CGEventRef where = CGEventCreate(NULL);
            pos = CGEventGetLocation(where);
            CFRelease(where);
        }
        e = CGEventCreateMouseEvent(src, kCGEventMouseMoved,
                                    CGPointMake(pos.x + (wig ? (sent & 1 ? wig : -wig) : 0), pos.y), 0);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
        sent++;
        if (sent % rate == 0) { printf("%d events\n", sent); fflush(stdout); }
        usleep(1000000 / rate);
    }
    return 0;
}
