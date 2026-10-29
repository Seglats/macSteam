/* Correct D3DMetal fullscreen windows left at the previous desktop size.
 * Runs in Steam's existing wrapper process; never changes display modes.
 * MACFINALS_DISPLAY_FIX lists the game executables to watch, separated by commas. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct { int32_t left, top, right, bottom; } MVDisplayRect;
static bool mvOversizedFullscreen(MVDisplayRect client, MVDisplayRect window,
                                  MVDisplayRect monitor, bool popup, bool caption) {
    int64_t width=(int64_t)monitor.right-monitor.left;
    int64_t height=(int64_t)monitor.bottom-monitor.top;
    return popup && !caption && width>0 && height>0 &&
        llabs((int64_t)window.left-monitor.left)<=2 &&
        llabs((int64_t)window.top-monitor.top)<=2 &&
        ((int64_t)client.right-client.left>width || (int64_t)client.bottom-client.top>height);
}

#ifndef MACVOLVO_DISPLAY_FIX_GEOMETRY_ONLY
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

typedef struct { HANDLE stop, thread; } MVDisplayWatcher;
static HWND mvDisplayWatchWindow;
static ULONGLONG mvDisplayWatchUntil, mvDisplayLastRepair;

static WCHAR mvDisplayGames[1024];

static BOOL mvGameWindow(HWND window) {
    if(!window || !IsWindowVisible(window) || GetAncestor(window,GA_ROOT)!=window)return FALSE;
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)return FALSE;
    WCHAR path[4096];DWORD size=4096;
    BOOL found=QueryFullProcessImageNameW(process,0,path,&size);
    CloseHandle(process);
    if(!found)return FALSE;
    WCHAR *name=wcsrchr(path,L'\\');name=name?name+1:path;
    WCHAR *forward=wcsrchr(name,L'/');if(forward)name=forward+1;
    size_t length=wcslen(name);
    for(const WCHAR *game=mvDisplayGames;*game;) {
        const WCHAR *end=wcschr(game,L',');
        size_t size=end?(size_t)(end-game):wcslen(game);
        if(size==length && !_wcsnicmp(game,name,length))return TRUE;
        if(!end)break;
        game=end+1;
    }
    return FALSE;
}

static MVDisplayRect mvDisplayRect(RECT rect) {
    return (MVDisplayRect){rect.left,rect.top,rect.right,rect.bottom};
}

static BOOL CALLBACK mvRepairGameWindow(HWND window,LPARAM unused) {
    (void)unused;
    if(!mvGameWindow(window))return TRUE;
    RECT client,outer;MONITORINFO monitor={0};monitor.cbSize=sizeof(monitor);
    if(!GetClientRect(window,&client) || !GetWindowRect(window,&outer) ||
       !GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor))return TRUE;
    LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
    if(!mvOversizedFullscreen(mvDisplayRect(client),mvDisplayRect(outer),mvDisplayRect(monitor.rcMonitor),
                             (style&WS_POPUP)!=0,(style&WS_CAPTION)!=0))return TRUE;
    ULONGLONG now=GetTickCount64();
    if(mvDisplayLastRepair && now-mvDisplayLastRepair<2000)return TRUE;
    LONG width=monitor.rcMonitor.right-monitor.rcMonitor.left;
    LONG height=monitor.rcMonitor.bottom-monitor.rcMonitor.top;
    if(SetWindowPos(window,NULL,monitor.rcMonitor.left,monitor.rcMonitor.top,width,height,
                    SWP_NOZORDER|SWP_NOACTIVATE|SWP_ASYNCWINDOWPOS)) {
        mvDisplayLastRepair=now;
        fprintf(stderr,"MacFinals: game fullscreen window %ldx%ld -> %ldx%ld\n",
                client.right-client.left,client.bottom-client.top,width,height);
        fflush(stderr);
    }
    return TRUE;
}

static void mvScheduleDisplayCheck(void) {
    if(!mvDisplayWatchWindow)return;
    mvDisplayWatchUntil=GetTickCount64()+3500;
    SetTimer(mvDisplayWatchWindow,1,200,NULL);
}

static LRESULT CALLBACK mvDisplayWatchProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_DISPLAYCHANGE)mvScheduleDisplayCheck();
    else if(message==WM_TIMER && wparam==1) {
        EnumWindows(mvRepairGameWindow,0);
        if(GetTickCount64()>=mvDisplayWatchUntil)KillTimer(window,1);
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

static void CALLBACK mvDisplayWindowEvent(HWINEVENTHOOK hook,DWORD event,HWND window,
                                         LONG object,LONG child,DWORD thread,DWORD time) {
    (void)hook;(void)event;(void)child;(void)thread;(void)time;
    if(object==OBJID_WINDOW && mvGameWindow(window))mvScheduleDisplayCheck();
}

static DWORD WINAPI mvWatchDisplay(void *argument) {
    HANDLE stop=argument;
    HANDLE mutex=CreateMutexW(NULL,FALSE,L"Local\\MacFinals.GameDisplay");
    if(!mutex)return 1;
    HANDLE waiting[]={stop,mutex};
    DWORD locked=WaitForMultipleObjects(2,waiting,FALSE,INFINITE);
    if(locked!=WAIT_OBJECT_0+1 && locked!=WAIT_ABANDONED_0+1) {CloseHandle(mutex);return 0;}
    /* Only this thread uses physical window coordinates; CEF keeps its own DPI context. */
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance=GetModuleHandleW(NULL);
    WNDCLASSW klass={0};klass.lpfnWndProc=mvDisplayWatchProc;
    klass.hInstance=instance;klass.lpszClassName=L"MacFinalsDisplayWatch";
    if(!RegisterClassW(&klass)) {ReleaseMutex(mutex);CloseHandle(mutex);return 1;}
    /* An invisible top-level window receives WM_DISPLAYCHANGE broadcasts. */
    mvDisplayWatchWindow=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,klass.lpszClassName,L"",
                                       WS_POPUP,0,0,0,0,NULL,NULL,instance,NULL);
    HWINEVENTHOOK hooks[3]={0};
    if(mvDisplayWatchWindow) {
        const DWORD events[]={EVENT_SYSTEM_FOREGROUND,EVENT_OBJECT_SHOW,EVENT_OBJECT_LOCATIONCHANGE};
        for(unsigned i=0;i<3;i++)hooks[i]=SetWinEventHook(events[i],events[i],NULL,mvDisplayWindowEvent,
                                                        0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        mvScheduleDisplayCheck();
        while(MsgWaitForMultipleObjects(1,&stop,FALSE,INFINITE,QS_ALLINPUT)==WAIT_OBJECT_0+1) {
            MSG message;
            while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)) {
                TranslateMessage(&message);DispatchMessageW(&message);
            }
        }
        for(unsigned i=0;i<3;i++)if(hooks[i])UnhookWinEvent(hooks[i]);
        KillTimer(mvDisplayWatchWindow,1);DestroyWindow(mvDisplayWatchWindow);mvDisplayWatchWindow=NULL;
    }
    UnregisterClassW(klass.lpszClassName,instance);
    ReleaseMutex(mutex);CloseHandle(mutex);return 0;
}

static MVDisplayWatcher mvStartDisplayWatcher(void) {
    MVDisplayWatcher watcher={0};
    DWORD size=GetEnvironmentVariableW(L"MACFINALS_DISPLAY_FIX",mvDisplayGames,1024);
    if(!size || size>=1024) {mvDisplayGames[0]=0;return watcher;}
    watcher.stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(watcher.stop)watcher.thread=CreateThread(NULL,0,mvWatchDisplay,watcher.stop,0,NULL);
    if(!watcher.thread && watcher.stop) {CloseHandle(watcher.stop);watcher.stop=NULL;}
    return watcher;
}

static void mvStopDisplayWatcher(MVDisplayWatcher watcher) {
    if(!watcher.thread)return;
    SetEvent(watcher.stop);
    /* No game code runs on the watcher thread; window moves are asynchronous. */
    WaitForSingleObject(watcher.thread,INFINITE);
    CloseHandle(watcher.thread);CloseHandle(watcher.stop);
}
#endif
