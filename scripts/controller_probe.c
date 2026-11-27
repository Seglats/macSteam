/* controller_probe.exe: what a Windows game sees of the controllers, run by macSteam's controller
 * check (app/Sources/ControllerCheck.swift) with the SteamDriver tool's Wine.
 *
 * Prints, one line each, to stdout:
 *   dinput <vid> <pid> <name>                 every DirectInput game controller, once at start
 *   x <pad> <buttons> <lt> <rt> <lx> <ly> <rx> <ry>   XInput pad 0-3 whenever its state changes
 *   x <pad> off                               a pad that was connected is gone
 *   ready                                     after the first full pass
 * Runs for the given seconds (default 600) or until stdin closes.
 * Build: x86_64-w64-mingw32-clang -O2 controller_probe.c -o controller_probe.exe -ldinput8 -ldxguid -lole32 */
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } PAD;
typedef struct { DWORD dwPacketNumber; PAD Gamepad; } STATE;
typedef DWORD (WINAPI *GETSTATE)(DWORD, STATE *);

/* Each DirectInput controller is printed once, when first seen: in a new prefix Wine's controller
 * bus comes up after the probe starts, so the list is checked again every 2 seconds. */
static GUID seen[32];
static int n_seen;
static BOOL CALLBACK on_device(const DIDEVICEINSTANCEA *inst, void *ctx)
{
    DWORD id = inst->guidProduct.Data1;
    for (int i = 0; i < n_seen; i++) if (IsEqualGUID(&seen[i], &inst->guidInstance)) return DIENUM_CONTINUE;
    if (n_seen < 32) seen[n_seen++] = inst->guidInstance;
    printf("dinput %04x %04x %s\n", (unsigned)LOWORD(id), (unsigned)HIWORD(id), inst->tszProductName);
    return DIENUM_CONTINUE;
}

static volatile LONG stop;
static DWORD WINAPI watch_stdin(void *arg)
{
    char buf[64];
    DWORD got;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    while (ReadFile(in, buf, sizeof(buf), &got, NULL) && got) {}
    InterlockedExchange(&stop, 1);
    return 0;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 600;
    IDirectInput8A *di = NULL;
    if (FAILED(DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&di, NULL)))
    {
        printf("dinput unavailable\n");
        di = NULL;
    }
    else IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, on_device, NULL, DIEDFL_ATTACHEDONLY);

    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (!h) h = LoadLibraryA("xinput1_3.dll");
    GETSTATE get = h ? (GETSTATE)GetProcAddress(h, "XInputGetState") : NULL;
    if (!get) { printf("xinput unavailable\n"); fflush(stdout); return 1; }

    CreateThread(NULL, 0, watch_stdin, NULL, 0, NULL);
    PAD last[4];
    int connected[4] = {0};
    memset(last, 0, sizeof(last));
    DWORD end = GetTickCount() + secs * 1000;
    for (int pass = 0; !stop && (int)(end - GetTickCount()) > 0; pass++)
    {
        for (int i = 0; i < 4; i++)
        {
            STATE s;
            if (get(i, &s) == ERROR_SUCCESS)
            {
                if (!connected[i] || memcmp(&s.Gamepad, &last[i], sizeof(PAD)))
                    printf("x %d %04x %u %u %d %d %d %d\n", i, s.Gamepad.wButtons, s.Gamepad.bLeftTrigger,
                           s.Gamepad.bRightTrigger, s.Gamepad.sThumbLX, s.Gamepad.sThumbLY,
                           s.Gamepad.sThumbRX, s.Gamepad.sThumbRY);
                last[i] = s.Gamepad;
                connected[i] = 1;
            }
            else if (connected[i]) { printf("x %d off\n", i); connected[i] = 0; }
        }
        if (pass == 0) printf("ready\n");
        if (di && pass % 400 == 399) IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, on_device, NULL, DIEDFL_ATTACHEDONLY);
        fflush(stdout);
        Sleep(5);
    }
    return 0;
}
