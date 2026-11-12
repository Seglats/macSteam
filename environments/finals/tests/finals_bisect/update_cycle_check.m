// Does AppKit's display-synchronized event loop delay input, and can a bare executable turn it off?
//
// Built like the finals Wine (update_cycle_check.py): no app bundle, x86_64, SDK 26.5. With
// UPDATE_CYCLE=off, NSApplicationUpdateCycleEnabled is registered as NO before NSApplication
// exists, which is what the Mac driver would do. Records from -[NSApplication sendEvent:], the
// point where Wine's WineApplication sees events, with the pointer frozen as in game.
//
// For each event: age = time received - the event's own hardware timestamp. Keys are timed only;
// which key was pressed is never read. Prints one JSON line.

#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#include <stdatomic.h>

enum { KIND_MOVE, KIND_BUTTON, KIND_KEY, KINDS };
enum { SECONDS = 15, BUCKETS = SECONDS * 10, CAPACITY = 1 << 18 };
static const char *kind_names[KINDS] = { "move", "button", "key" };

static float *ages[KINDS];
static size_t counts[KINDS];
static atomic_int recording;
static atomic_ullong gc_moves;
static atomic_uchar active[BUCKETS + 1];
static double started;
static NSTextField *label;

static double now(void) { return NSProcessInfo.processInfo.systemUptime; }

static int compare(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void finish(void)
{
    unsigned buckets = 0;
    const char *mode = getenv("UPDATE_CYCLE");

    atomic_store(&recording, 0);
    CGAssociateMouseAndMouseCursorPosition(YES);
    for (int i = 0; i < BUCKETS; i++) buckets += atomic_load(&active[i]);
    double seconds = buckets ? buckets / 10.0 : 1;
    printf("{\"mode\":\"%s\",\"moving_s\":%.1f,\"gc_per_s\":%.1f,\"appkit_per_s\":%.1f", mode ? mode : "default",
           seconds, atomic_load(&gc_moves) / seconds, counts[KIND_MOVE] / seconds);
    for (int k = 0; k < KINDS; k++)
    {
        size_t n = counts[k];
        qsort(ages[k], n, sizeof(float), compare);
        printf(",\"%s_n\":%zu,\"%s_median_ms\":%.3f,\"%s_p95_ms\":%.3f,\"%s_max_ms\":%.3f", kind_names[k], n,
               kind_names[k], n ? ages[k][n / 2] : -1.0, kind_names[k], n ? ages[k][n * 95 / 100] : -1.0,
               kind_names[k], n ? ages[k][n - 1] : -1.0);
    }
    printf("}\n");
    fflush(stdout);
    _exit(0);
}

static void begin(void)
{
    NSWindow *window = NSApp.keyWindow;
    if (window)
    {
        NSRect frame = window.frame;
        CGFloat top = NSScreen.screens[0].frame.size.height;
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(frame), top - NSMidY(frame)));
    }
    CGAssociateMouseAndMouseCursorPosition(NO);
    started = now();
    atomic_store(&recording, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, SECONDS * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ finish(); });
}

@interface CheckApp : NSApplication
@end
@implementation CheckApp
- (void)sendEvent:(NSEvent *)event
{
    double received = now();
    NSEventType type = event.type;
    int kind = -1;

    if (type == NSEventTypeMouseMoved || type == NSEventTypeLeftMouseDragged ||
        type == NSEventTypeRightMouseDragged || type == NSEventTypeOtherMouseDragged)
        kind = KIND_MOVE;
    else if (type == NSEventTypeLeftMouseDown || type == NSEventTypeRightMouseDown || type == NSEventTypeOtherMouseDown)
        kind = KIND_BUTTON;
    else if (type == NSEventTypeKeyDown && !event.isARepeat)
        kind = KIND_KEY;

    if (atomic_load(&recording))
    {
        if (kind >= 0 && counts[kind] < CAPACITY)
            ages[kind][counts[kind]++] = (float)((received - event.timestamp) * 1e3);
    }
    else if (kind == KIND_BUTTON && !started)
    {
        begin();
        return;
    }
    [super sendEvent:event];
}
@end

@interface CheckView : NSView
@end
@implementation CheckView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)event { }
- (void)keyUp:(NSEvent *)event { }
- (void)tick:(CADisplayLink *)link
{
    /* Something on screen changes every refresh, as in a game. */
    label.stringValue = started ? [NSString stringWithFormat:@"Keep moving the mouse, clicking and tapping keys: %.1f s",
                                                             MAX(0.0, started + SECONDS - now())]
                                : @"Click here to start this round";
}
@end

static void attach(GCMouse *mouse)
{
    static dispatch_queue_t queue;
    if (!queue)
        queue = dispatch_queue_create("check.gcmouse", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                                               QOS_CLASS_USER_INTERACTIVE, 0));
    if ([mouse.vendorName rangeOfString:@"Trackpad" options:NSCaseInsensitiveSearch].location != NSNotFound) return;
    mouse.handlerQueue = queue;
    mouse.mouseInput.mouseMovedHandler = ^(GCMouseInput *input, float dx, float dy) {
        if (!atomic_load(&recording)) return;
        atomic_fetch_add(&gc_moves, 1);
        int bucket = (int)((now() - started) * 10);
        if (bucket >= 0 && bucket < BUCKETS) atomic_store(&active[bucket], 1);
    };
}

int main(void)
{
    @autoreleasepool {
        const char *mode = getenv("UPDATE_CYCLE");

        for (int k = 0; k < KINDS; k++) ages[k] = malloc(CAPACITY * sizeof(float));
        if (mode && !strcmp(mode, "off"))
            [[NSUserDefaults standardUserDefaults] registerDefaults:@{ @"NSApplicationUpdateCycleEnabled" : @NO }];

        [CheckApp sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(300, 300, 640, 200)
                                                       styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        CheckView *view = [[CheckView alloc] initWithFrame:NSMakeRect(0, 0, 640, 200)];
        label = [NSTextField labelWithString:@""];
        label.frame = NSMakeRect(20, 85, 600, 30);
        label.font = [NSFont systemFontOfSize:17];
        [view addSubview:label];
        window.contentView = view;
        window.title = @"Input timing check";
        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:view];
        [[view displayLinkWithTarget:view selector:@selector(tick:)] addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];

        for (GCMouse *mouse in GCMouse.mice) attach(mouse);
        [NSNotificationCenter.defaultCenter addObserverForName:GCMouseDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *note) { attach(note.object); }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            if (!started) { fprintf(stderr, "nobody clicked within 5 minutes\n"); _exit(2); }
        });
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}
