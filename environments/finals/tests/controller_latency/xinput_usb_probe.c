/* Controller latency probe for XInput pads on a cable: what Wine's Xbox 360 USB bus would get.
 *
 * Pads in XInput mode (USB interface class 255, subclass 93, protocol 1) are not HID, so macOS's HID
 * layer and SDL never see them (pad_probe.c finds nothing). winebus.sys has a bus for them
 * (dlls/winebus.sys/bus_xbox360.c, CrossOver's), but it only accepts Microsoft's vendor id and turns
 * itself off on macOS 15 and later. This probe opens such an interface the way that bus does
 * (IOUSBLib: USBInterfaceOpen, ReadPipeAsync on pipe 1 with a CFRunLoop source) for any vendor,
 * prints the endpoint's polling interval, and times every report.
 *
 *   xinput_usb_probe [seconds]
 *
 * Reads only. It sends nothing to the pad (no LED or rumble commands). */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_INTERVALS 65536
static double ticks_to_ms;
static double now_ms(void) { return mach_absolute_time() * ticks_to_ms; }

static IOUSBInterfaceInterface550 **intf;
static UInt8 in_pipe;
static UInt8 buf[64];
static double last_t, last_change_t;
static float idle[MAX_INTERVALS], busy[MAX_INTERVALS];
static int n_idle, n_busy;
static long reports, button_changes, stick_changes;
static UInt8 last[20];
static int have_last;

static void read_done(void *refcon, IOReturn result, void *arg0)
{
    UInt32 len = (UInt32)(uintptr_t)arg0;
    double t = now_ms();   /* completion on the run loop: the time the data is usable, as in winebus */
    if (result != kIOReturnSuccess)
    {
        fprintf(stderr, "read failed: 0x%x\n", result);
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    reports++;
    if (last_t > 0)
    {
        if (t - last_change_t < 100) { if (n_busy < MAX_INTERVALS) busy[n_busy++] = t - last_t; }
        else if (n_idle < MAX_INTERVALS) idle[n_idle++] = t - last_t;
    }
    last_t = t;
    /* Xbox 360 input report: [0]=0x00 [1]=0x14 [2..3]=buttons [4]=LT [5]=RT [6..13]=sticks (16-bit). */
    if (len >= 14 && buf[0] == 0x00 && buf[1] == 0x14)
    {
        if (have_last)
        {
            int b = memcmp(buf + 2, last + 2, 4) != 0;   /* buttons and triggers */
            int s = 0;
            for (int i = 0; i < 4; i++)
            {
                int now = (short)(buf[6 + 2 * i] | buf[7 + 2 * i] << 8), was = (short)(last[6 + 2 * i] | last[7 + 2 * i] << 8);
                if (abs(now - was) > 256) s = 1;
            }
            if (b) button_changes++;
            if (s) stick_changes++;
            if (b || s) last_change_t = t;
            static double last_print;
            if ((b || s) && t - last_print > 200)
            {
                last_print = t;
                printf("report  buttons %02x%02x  LT %3d RT %3d  L %6d,%6d  R %6d,%6d\n", buf[3], buf[2], buf[4], buf[5],
                       (short)(buf[6] | buf[7] << 8), (short)(buf[8] | buf[9] << 8),
                       (short)(buf[10] | buf[11] << 8), (short)(buf[12] | buf[13] << 8));
            }
        }
        memcpy(last, buf, 14);
        have_last = 1;
    }
    else
    {
        static int shown;
        if (shown++ < 5)
        {
            printf("other report (%u bytes):", len);
            for (UInt32 i = 0; i < len && i < 20; i++) printf(" %02x", buf[i]);
            printf("\n");
        }
    }
    IOReturn r = (*intf)->ReadPipeAsync(intf, in_pipe, buf, sizeof(buf), read_done, NULL);
    if (r != kIOReturnSuccess) { fprintf(stderr, "ReadPipeAsync: 0x%x\n", r); CFRunLoopStop(CFRunLoopGetCurrent()); }
}

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static void print_intervals(const char *label, float *v, int n)
{
    if (n <= 10) { printf("  report interval %s: too few reports (%d)\n", label, n); return; }
    qsort(v, n, sizeof(float), cmpf);
    float med = v[n / 2];
    printf("  report interval %s: median %.2f ms (%.0f Hz), p5 %.2f, p95 %.2f, p99 %.2f, max %.2f ms  (%d)\n",
           label, med, med > 0 ? 1000 / med : 0, v[n * 5 / 100], v[n * 95 / 100], v[n * 99 / 100], v[n - 1], n);
}

static void stop_timer(CFRunLoopTimerRef timer, void *info) { CFRunLoopStop(CFRunLoopGetCurrent()); }
static void on_signal(int s) { CFRunLoopStop(CFRunLoopGetMain()); }

static IOUSBInterfaceInterface550 **open_xinput(io_service_t *found_dev)
{
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostInterface"), &it) != KERN_SUCCESS) return NULL;
    io_service_t svc;
    while ((svc = IOIteratorNext(it)))
    {
        /* class 255, subclass 93, protocol 1: the Xbox 360 wired protocol */
        int cls = -1, sub = -1, proto = -1;
        const char *keys[3] = { "bInterfaceClass", "bInterfaceSubClass", "bInterfaceProtocol" };
        int *vals[3] = { &cls, &sub, &proto };
        for (int k = 0; k < 3; k++)
        {
            CFStringRef key = CFStringCreateWithCString(NULL, keys[k], kCFStringEncodingUTF8);
            CFNumberRef n = IORegistryEntryCreateCFProperty(svc, key, NULL, 0);
            if (n) { CFNumberGetValue(n, kCFNumberIntType, vals[k]); CFRelease(n); }
            CFRelease(key);
        }
        if (cls != 255 || sub != 93 || proto != 1) { IOObjectRelease(svc); continue; }
        io_name_t name;
        IORegistryEntryGetName(svc, name);
        io_service_t parent = 0;
        IORegistryEntryGetParentEntry(svc, kIOServicePlane, &parent);
        CFNumberRef vidref = parent ? IORegistryEntryCreateCFProperty(parent, CFSTR("idVendor"), NULL, 0) : NULL;
        CFNumberRef pidref = parent ? IORegistryEntryCreateCFProperty(parent, CFSTR("idProduct"), NULL, 0) : NULL;
        CFStringRef prod = parent ? IORegistryEntryCreateCFProperty(parent, CFSTR("USB Product Name"), NULL, 0) : NULL;
        int vid = 0, pid = 0;
        char pname[128] = "?";
        if (vidref) CFNumberGetValue(vidref, kCFNumberIntType, &vid);
        if (pidref) CFNumberGetValue(pidref, kCFNumberIntType, &pid);
        if (prod) CFStringGetCString(prod, pname, sizeof(pname), kCFStringEncodingUTF8);
        printf("XInput interface: %s (%04x:%04x)\n", pname, vid, pid);

        IOCFPlugInInterface **plug = NULL;
        SInt32 score;
        IOReturn r = IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
        if (r != kIOReturnSuccess || !plug) { printf("  plug-in: 0x%x\n", r); continue; }
        IOUSBInterfaceInterface550 **i = NULL;
        (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID550), (LPVOID *)&i);
        (*plug)->Release(plug);
        if (!i) { printf("  no interface object\n"); continue; }
        r = (*i)->USBInterfaceOpen(i);
        if (r != kIOReturnSuccess)
        {
            printf("  USBInterfaceOpen: 0x%x%s\n", r, r == kIOReturnExclusiveAccess ? " (another program holds it exclusively)" : "");
            r = (*i)->USBInterfaceOpenSeize(i);
            printf("  USBInterfaceOpenSeize: 0x%x\n", r);
            if (r != kIOReturnSuccess) continue;
        }
        UInt8 n_ep = 0;
        (*i)->GetNumEndpoints(i, &n_ep);
        for (UInt8 pipe = 1; pipe <= n_ep; pipe++)
        {
            UInt8 dir, num, type, interval;
            UInt16 maxp;
            (*i)->GetPipeProperties(i, pipe, &dir, &num, &type, &maxp, &interval);
            printf("  pipe %u: endpoint %u %s %s, max packet %u, bInterval %u%s\n", pipe, num,
                   dir == kUSBIn ? "IN" : "OUT", type == kUSBInterrupt ? "interrupt" : "other", maxp, interval,
                   type == kUSBInterrupt ? " (full speed: ms between polls)" : "");
            if (dir == kUSBIn && type == kUSBInterrupt && !in_pipe) in_pipe = pipe;
        }
        if (!in_pipe) { printf("  no interrupt IN pipe\n"); (*i)->USBInterfaceClose(i); continue; }
        if (in_pipe != 1) printf("  note: the IN pipe is %u; Wine's bus always reads pipe 1\n", in_pipe);
        return i;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    ticks_to_ms = (double)tb.numer / tb.denom / 1e6;
    io_service_t dev = 0;
    intf = open_xinput(&dev);
    if (!intf) { printf("no openable XInput-class interface found\n"); return 1; }
    if (argc > 2 && !strcmp(argv[2], "--init"))
    {
        /* The control read Windows' XUSB driver makes first (Linux xpad, xpad_start_xbox_360): many
         * non-Microsoft pads send no input until they see it. IN, vendor, interface; reads 20 bytes. */
        UInt8 data[20] = {0};
        IOUSBDevRequest req = { .bmRequestType = USBmakebmRequestType(kUSBIn, kUSBVendor, kUSBInterface),
                                .bRequest = 0x01, .wValue = 0x0100, .wIndex = 0, .wLength = sizeof(data), .pData = data };
        IOReturn ir = (*intf)->ControlRequest(intf, 0, &req);
        printf("init control read (0xc1 0x01 0x0100): 0x%x, %u bytes:", ir, (unsigned)req.wLenDone);
        for (UInt32 k = 0; k < req.wLenDone; k++) printf(" %02x", data[k]);
        printf("\n");
        /* Then the player-LED command Linux xpad sends to every Xbox 360 pad (xpad_identify_controller):
         * 01 03 <pattern>, 0x02 = player 1 flashes then stays on. The only write this probe makes. */
        UInt8 led[3] = { 0x01, 0x03, 0x02 };
        UInt8 n_ep = 0;
        (*intf)->GetNumEndpoints(intf, &n_ep);
        for (UInt8 pipe = 1; pipe <= n_ep; pipe++)
        {
            UInt8 dir, num, type, interval;
            UInt16 maxp;
            (*intf)->GetPipeProperties(intf, pipe, &dir, &num, &type, &maxp, &interval);
            if (dir == kUSBOut && type == kUSBInterrupt)
            {
                IOReturn lr = (*intf)->WritePipe(intf, pipe, led, sizeof(led));
                printf("player LED command on pipe %u: 0x%x\n", pipe, lr);
                break;
            }
        }
    }
    CFRunLoopSourceRef src;
    (*intf)->CreateInterfaceAsyncEventSource(intf, &src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopCommonModes);
    IOReturn r = (*intf)->ReadPipeAsync(intf, in_pipe, buf, sizeof(buf), read_done, NULL);
    if (r != kIOReturnSuccess) { printf("ReadPipeAsync: 0x%x\n", r); return 1; }
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + secs, 0, 0, 0, stop_timer, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
    signal(SIGINT, on_signal);
    printf("reading for %d s: tap buttons, pull the triggers, move both sticks to the edges\n", secs);
    CFRunLoopRun();
    printf("\n==== summary ====\n%ld reports, %ld with button/trigger changes, %ld with stick changes\n",
           reports, button_changes, stick_changes);
    print_intervals("untouched", idle, n_idle);
    print_intervals("in use   ", busy, n_busy);
    (*intf)->USBInterfaceClose(intf);
    return 0;
}
