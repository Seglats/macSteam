/* Prints what a Windows program gets from XInput inside Wine: sticks, triggers, buttons of pad 0,
 * every 250 ms for the given seconds. XInput convention: thumb Y is positive when pushed UP.
 *   wine xinput_state_probe.exe [seconds]
 * Build: x86_64-w64-mingw32-gcc -O2 -o xinput_state_probe.exe xinput_state_probe.c */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } PAD;
typedef struct { DWORD dwPacketNumber; PAD Gamepad; } STATE;
typedef DWORD (WINAPI *GETSTATE)(DWORD, STATE *);

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (!h) h = LoadLibraryA("xinput1_3.dll");
    GETSTATE get = h ? (GETSTATE)GetProcAddress(h, "XInputGetState") : NULL;
    if (!get) { printf("no XInputGetState\n"); return 1; }
    for (int i = 0; i < secs * 4; i++)
    {
        STATE s;
        DWORD r = get(0, &s);
        if (r) printf("pad 0: not connected (%lu)\n", (unsigned long)r);
        else printf("pad 0: L %6d,%6d  R %6d,%6d  LT %3u RT %3u  buttons %04x\n", s.Gamepad.sThumbLX, s.Gamepad.sThumbLY,
                    s.Gamepad.sThumbRX, s.Gamepad.sThumbRY, s.Gamepad.bLeftTrigger, s.Gamepad.bRightTrigger, s.Gamepad.wButtons);
        fflush(stdout);
        Sleep(250);
    }
    return 0;
}
