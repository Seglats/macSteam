/* What status does a system call return when the handle is the wrong kind of object?
 * Wine decides this in server/handle.c get_handle_obj(): a handle missing from the table
 * gives STATUS_INVALID_HANDLE, while a pseudo-handle resolving to the wrong type gives
 * STATUS_OBJECT_TYPE_MISMATCH. Real Windows answers c0000008 where we answer c0000024 for
 * at least one case, so this walks the whole matrix to establish the actual rule before
 * anything in Wine is changed.
 *
 * Build: x86_64-w64-mingw32-clang -O1 handle_probe.c -o handle-probe.exe -lntdll
 * Run the same .exe on MacFinals, real Windows and Proton and compare the tables. */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

NTSTATUS NTAPI NtWriteFile(HANDLE,HANDLE,PVOID,PVOID,IO_STATUS_BLOCK *,const void *,ULONG,LARGE_INTEGER *,ULONG *);
NTSTATUS NTAPI NtSetEvent(HANDLE,LONG *);

/* A file operation on `h`: expects a File object. */
static NTSTATUS try_file(HANDLE h) {
    IO_STATUS_BLOCK io;LARGE_INTEGER off={0};BYTE buf[4]={0};
    return NtWriteFile(h,NULL,NULL,NULL,&io,buf,sizeof buf,&off,NULL);
}
/* A process query on `h`: expects a Process object. */
static NTSTATUS try_process(HANDLE h) {
    PROCESS_BASIC_INFORMATION pbi;
    return NtQueryInformationProcess(h,ProcessBasicInformation,&pbi,sizeof pbi,NULL);
}
/* An event signal on `h`: expects an Event object. */
static NTSTATUS try_event(HANDLE h) { return NtSetEvent(h,NULL); }

int main(void) {
    WCHAR dir[MAX_PATH],path[MAX_PATH];
    GetTempPathW(MAX_PATH,dir);GetTempFileNameW(dir,L"hp",0,path);
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,NULL);
    HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);
    HANDLE closed=CreateEventW(NULL,TRUE,FALSE,NULL);CloseHandle(closed);

    struct { const char *name; HANDLE h; } handles[] = {
        {"process pseudo (-1)", GetCurrentProcess()},
        {"thread pseudo (-2)",  GetCurrentThread()},
        {"real file",           file},
        {"real event",          event},
        {"closed event",        closed},
        {"garbage 0x1234",      (HANDLE)(ULONG_PTR)0x1234},
        {"NULL",                NULL},
    };
    struct { const char *name; NTSTATUS (*fn)(HANDLE); } ops[] = {
        {"NtWriteFile",                try_file},
        {"NtQueryInformationProcess",  try_process},
        {"NtSetEvent",                 try_event},
    };

    printf("%-22s", "handle \\ call");
    for (unsigned o=0;o<3;o++) printf(" %-28s", ops[o].name);
    printf("\n");
    for (unsigned i=0;i<sizeof handles/sizeof *handles;i++) {
        printf("%-22s", handles[i].name);
        for (unsigned o=0;o<3;o++) {
            NTSTATUS s=ops[o].fn(handles[i].h);
            char cell[32];snprintf(cell,sizeof cell,"%08lx",(unsigned long)s);
            printf(" %-28s", cell);
        }
        printf("\n");
    }
    printf("\nc0000008 INVALID_HANDLE  c0000024 OBJECT_TYPE_MISMATCH  c0000005 ACCESS_VIOLATION\n");
    printf("c000000d INVALID_PARAMETER  00000000 SUCCESS\n");
    CloseHandle(file);CloseHandle(event);
    return 0;
}
