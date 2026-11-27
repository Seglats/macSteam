/* hotplug_probe.exe: which device announcements a game window gets when a controller is plugged
 * in (or switched on) while it runs, under Wine or Windows.
 *
 * A visible top-level window, like a game's, that:
 *   - takes WM_DEVICECHANGE (every top-level window gets DBT_DEVNODES_CHANGED on Windows;
 *     DBT_DEVICEARRIVAL/REMOVECOMPLETE come because it registers for all interface classes);
 *   - registers raw input for joysticks and gamepads with RIDEV_DEVNOTIFY (WM_INPUT_DEVICE_CHANGE);
 *   - polls XInputGetState for pads 0-3 every 4 ms;
 *   - on each DBT_DEVNODES_CHANGED, asks XInput about every slot once right away, as Unreal
 *     Engine does: "CONNECTED" there means a game checking only then would see the controller.
 * Each line carries milliseconds since start. Usage: hotplug_probe.exe [seconds, default 60] [nopoll]
 *
 * Build: x86_64-w64-mingw32-gcc -O2 hotplug_probe.c -o hotplug_probe.exe -luser32 -lgdi32
 */
#include <windows.h>
#include <dbt.h>
#include <stdio.h>
#include <string.h>

typedef struct { WORD b; BYTE lt, rt; SHORT lx, ly, rx, ry; } PAD;
typedef struct { DWORD packet; PAD pad; } STATE;
typedef DWORD (WINAPI *GETSTATE)(DWORD, STATE *);

static LARGE_INTEGER freq, start;
static GETSTATE get_state;
static int connected[4];

static double ms(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
}

static void poll_xinput(void)
{
    for (DWORD i = 0; i < 4; i++)
    {
        STATE s;
        int on = get_state && get_state(i, &s) == ERROR_SUCCESS;
        if (on != connected[i]) printf("%9.1f xinput pad %lu %s\n", ms(), i, on ? "connected" : "gone");
        connected[i] = on;
    }
}

static const char *devchange_name(WPARAM w)
{
    switch (w)
    {
    case DBT_DEVNODES_CHANGED: return "DBT_DEVNODES_CHANGED";
    case DBT_DEVICEARRIVAL: return "DBT_DEVICEARRIVAL";
    case DBT_DEVICEREMOVECOMPLETE: return "DBT_DEVICEREMOVECOMPLETE";
    default: return "other";
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_DEVICECHANGE:
    {
        const DEV_BROADCAST_HDR *hdr = (const DEV_BROADCAST_HDR *)l;
        printf("%9.1f WM_DEVICECHANGE %s (%#x)", ms(), devchange_name(w), (unsigned)w);
        if ((w == DBT_DEVICEARRIVAL || w == DBT_DEVICEREMOVECOMPLETE) && hdr
            && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
            printf(" %ls", ((const DEV_BROADCAST_DEVICEINTERFACE_W *)l)->dbcc_name);
        printf("\n");
        /* What Unreal Engine does: on WM_DEVICECHANGE, ask XInput about every slot once. */
        if (w == DBT_DEVNODES_CHANGED && get_state)
        {
            printf("%9.1f   check on this message:", ms());
            for (DWORD i = 0; i < 4; i++)
            {
                STATE s;
                printf(" %lu %s", i, get_state(i, &s) == ERROR_SUCCESS ? "CONNECTED" : "-");
            }
            printf("\n");
        }
        return TRUE;
    }
    case WM_INPUT_DEVICE_CHANGE:
    {
        WCHAR name[512] = L"";
        UINT size = ARRAYSIZE(name);
        GetRawInputDeviceInfoW((HANDLE)l, RIDI_DEVICENAME, name, &size);
        printf("%9.1f WM_INPUT_DEVICE_CHANGE %s %ls\n", ms(), w == GIDC_ARRIVAL ? "GIDC_ARRIVAL" : "GIDC_REMOVAL", name);
        return 0;
    }
    case WM_TIMER:
        poll_xinput();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    setvbuf(stdout, NULL, _IONBF, 0);

    HMODULE x = LoadLibraryA("xinput1_4.dll");
    get_state = x ? (GETSTATE)GetProcAddress(x, "XInputGetState") : NULL;

    WNDCLASSW wc = { .lpfnWndProc = wndproc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"hotplug_probe" };
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"hotplug_probe", L"hotplug probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              40, 40, 320, 120, NULL, NULL, wc.hInstance, NULL);

    DEV_BROADCAST_DEVICEINTERFACE_W filter = { .dbcc_size = sizeof(filter), .dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE };
    if (!RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES))
        printf("RegisterDeviceNotification failed %lu\n", GetLastError());

    RAWINPUTDEVICE rid[2] = {
        { 0x01, 0x04, RIDEV_DEVNOTIFY | RIDEV_INPUTSINK, hwnd },  /* joystick */
        { 0x01, 0x05, RIDEV_DEVNOTIFY | RIDEV_INPUTSINK, hwnd },  /* gamepad */
    };
    if (!RegisterRawInputDevices(rid, 2, sizeof(rid[0])))
        printf("RegisterRawInputDevices failed %lu\n", GetLastError());

    poll_xinput();
    printf("%9.1f ready: plug in or switch on the controller now (%d s)\n", ms(), secs);
    /* "nopoll": no 4 ms polling, so only the check on each message asks XInput, like Unreal. */
    if (!(argc > 2 && !strcmp(argv[2], "nopoll"))) SetTimer(hwnd, 1, 4, NULL);
    SetTimer(hwnd, 2, secs * 1000, NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (m.message == WM_TIMER && m.wParam == 2) break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    printf("%9.1f done\n", ms());
    return 0;
}
