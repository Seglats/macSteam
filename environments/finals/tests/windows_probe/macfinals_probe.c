/* Windows conformance probe for MacFinals' runtime: what an x86-64 Windows program sees of
 * the CPU (through Rosetta) and of the process (through Wine), next to what real hardware
 * and Windows give. Run the same .exe on Windows to confirm the expected values.
 *
 * Build: x86_64-w64-mingw32-clang -O1 macfinals_probe.c -o macfinals-probe.exe -lntdll
 * Lines: "ok" matches Windows, "DIFF" doesn't, "info" only reports (no fixed Windows value). */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <winternl.h>
#include <cpuid.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int same, const char *what, const char *got, const char *windows) {
    printf("%-4s %-46s got %-24s windows %s\n", same ? "ok" : "DIFF", what, got, windows);
    if(!same)failures++;
}
static void info(const char *what, const char *value) {printf("info %-46s %s\n", what, value);}

/* Exceptions the CPU tests expect: the handler records the code and resumes at a label. */
static volatile DWORD caught;
static volatile DWORD64 caught_eflags;
static void *volatile resume_at;
static volatile LONG64 resume_rsp_adjust;
static volatile int inject_nt; /* resume with NT set, to test the context-restore path */
static LONG CALLBACK handler(EXCEPTION_POINTERS *e) {
    if(!resume_at)return EXCEPTION_CONTINUE_SEARCH;
    caught=e->ExceptionRecord->ExceptionCode;caught_eflags=e->ContextRecord->EFlags;
    e->ContextRecord->Rip=(DWORD64)resume_at;e->ContextRecord->Rsp+=resume_rsp_adjust;
    if(inject_nt)e->ContextRecord->EFlags|=0x4000;
    else e->ContextRecord->EFlags&=~0x4000; /* never resume with NT set */
    resume_at=NULL;
    return EXCEPTION_CONTINUE_EXECUTION;
}

extern char after_int3[], after_iret[], after_inject[];
#define NT_FLAG 0x4000

static void cpu_identity(void) {
    unsigned a,b,c,d;char vendor[13]={0},brand[49]={0},line[96];
    __cpuid(0,a,b,c,d);memcpy(vendor,&b,4);memcpy(vendor+4,&d,4);memcpy(vendor+8,&c,4);
    info("CPUID vendor",vendor);
    for(unsigned i=0;i<3;i++){__cpuid(0x80000002+i,a,b,c,d);memcpy(brand+16*i,&a,4);memcpy(brand+16*i+4,&b,4);memcpy(brand+16*i+8,&c,4);memcpy(brand+16*i+12,&d,4);}
    info("CPUID brand",brand);
    __cpuid(1,a,b,c,d);
    snprintf(line,sizeof line,"family %u model %u stepping %u",((a>>8)&15)+((a>>20)&255),((a>>4)&15)|((a>>12)&0xf0),a&15);info("CPUID signature",line);
    snprintf(line,sizeof line,"hypervisor %u avx %u osxsave %u",(c>>31)&1,(c>>28)&1,(c>>27)&1);info("CPUID leaf 1 flags",line);
    __cpuid_count(7,0,a,b,c,d);
    snprintf(line,sizeof line,"avx2 %u fsgsbase %u",(b>>5)&1,b&1);info("CPUID leaf 7 flags",line);
}

/* The NT (nested task) flag is an ordinary user-settable flag on x86-64 hardware. */
static void nt_flag(void) {
    DWORD64 flags;char got[32];
    __asm__ volatile("pushfq\n\torq $0x4000,(%%rsp)\n\tpopfq\n\tpushfq\n\tpopq %0\n\t"
                     "pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq" : "=r"(flags) :: "memory","cc");
    snprintf(got,sizeof got,"NT=%u",(unsigned)!!(flags&NT_FLAG));
    check(!!(flags&NT_FLAG),"POPF sets NT, PUSHF reads it back",got,"NT=1");

    /* An exception raised with NT set: the thread context must carry it. */
    caught=0;caught_eflags=0;resume_rsp_adjust=0;resume_at=after_int3;
    __asm__ volatile("pushfq\n\torq $0x4000,(%%rsp)\n\tpopfq\n\tint3\n\t"
                     ".globl after_int3\nafter_int3:\n\t"
                     "pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq" ::: "memory","cc");
    snprintf(got,sizeof got,"code %08lx NT=%u",caught,(unsigned)!!(caught_eflags&NT_FLAG));
    check(caught==EXCEPTION_BREAKPOINT && (caught_eflags&NT_FLAG),"exception context keeps NT",got,"code 80000003 NT=1");

    /* IRETQ with NT set is a task return, which 64-bit mode refuses with #GP. */
    caught=0;resume_rsp_adjust=40;resume_at=after_iret;
    __asm__ volatile("pushfq\n\torq $0x4000,(%%rsp)\n\tpopfq\n\t"
                     "movq %%rsp,%%rax\n\tmovl %%ss,%%ecx\n\tpushq %%rcx\n\tpushq %%rax\n\tpushfq\n\t"
                     "movl %%cs,%%ecx\n\tpushq %%rcx\n\tleaq after_iret(%%rip),%%rcx\n\tpushq %%rcx\n\tiretq\n\t"
                     ".globl after_iret\nafter_iret:\n\t"
                     "pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq" ::: "rax","rcx","memory","cc");
    resume_at=NULL;
    snprintf(got,sizeof got,"code %08lx",caught);
    check(caught==EXCEPTION_ACCESS_VIOLATION,"IRETQ with NT set faults (#GP)",got,"c0000005");

    /* NT set in a thread context, then resumed. Wine carries this correctly: the
     * andq $~0x4000 before its iretq (signal_x86_64.c) only keeps NT out of the flags live
     * during the iretq itself, which would fault; the restored EFLAGS come from the iret
     * frame. Proton on real hardware answers NT=1 here with that same Wine code, so NT=0
     * means the translator does not model the flag, and no Wine change can alter it. */
    caught=0;flags=0;resume_rsp_adjust=0;inject_nt=1;resume_at=after_inject;
    __asm__ volatile("int3\n\t"
                     ".globl after_inject\nafter_inject:\n\tpushfq\n\tpopq %0\n\t"
                     "pushfq\n\tandq $~0x4000,(%%rsp)\n\tpopfq" : "=r"(flags) :: "memory","cc");
    inject_nt=0;
    snprintf(got,sizeof got,"NT=%u",(unsigned)!!(flags&NT_FLAG));
    check(!!(flags&NT_FLAG),"NT injected via exception context survives",got,"NT=1");
}

/* Fields anti-debug code reads from the PEB, and the debugger queries. */
static void process_state(void) {
    BYTE *peb=(BYTE *)NtCurrentTeb()->ProcessEnvironmentBlock;char got[48];
    snprintf(got,sizeof got,"%u",peb[2]);check(peb[2]==0,"PEB BeingDebugged",got,"0");
    ULONG global=*(ULONG *)(peb+0xbc);snprintf(got,sizeof got,"%08lx",global);check(global==0,"PEB NtGlobalFlag",got,"00000000");
    BYTE *heap=*(BYTE **)(peb+0x30);
    ULONG flags=*(ULONG *)(heap+0x70),force=*(ULONG *)(heap+0x74);
    snprintf(got,sizeof got,"%08lx",flags);check(flags==HEAP_GROWABLE,"process heap Flags (heap+0x70)",got,"00000002");
    snprintf(got,sizeof got,"%08lx",force);check(force==0,"process heap ForceFlags (heap+0x74)",got,"00000000");
    BYTE *params=*(BYTE **)(peb+0x20);
    /* 0x4000: no Image File Execution Options key for this program (Windows and Proton set it) */
    ULONG pflags=*(ULONG *)(params+8),pdebug=*(ULONG *)(params+12);
    snprintf(got,sizeof got,"Flags %08lx DebugFlags %08lx",pflags,pdebug);
    check(pflags==0x4001 && !pdebug,"ProcessParameters",got,"Flags 00004001 DebugFlags 00000000");

    ULONG_PTR port=1;NTSTATUS s=NtQueryInformationProcess(GetCurrentProcess(),(PROCESSINFOCLASS)7,&port,sizeof port,NULL);
    snprintf(got,sizeof got,"%08lx port %llu",s,(unsigned long long)port);check(!s && !port,"ProcessDebugPort",got,"00000000 port 0");
    HANDLE object=(HANDLE)1;s=NtQueryInformationProcess(GetCurrentProcess(),(PROCESSINFOCLASS)0x1e,&object,sizeof object,NULL);
    snprintf(got,sizeof got,"%08lx",s);check(s==(NTSTATUS)0xc0000353,"ProcessDebugObjectHandle",got,"c0000353 (port not set)");
    ULONG inherit=0;s=NtQueryInformationProcess(GetCurrentProcess(),(PROCESSINFOCLASS)0x1f,&inherit,sizeof inherit,NULL);
    snprintf(got,sizeof got,"%08lx value %lu",s,inherit);check(!s && inherit==1,"ProcessDebugFlags",got,"00000000 value 1");

    CONTEXT ctx={.ContextFlags=CONTEXT_DEBUG_REGISTERS};GetThreadContext(GetCurrentThread(),&ctx);
    snprintf(got,sizeof got,"%llx %llx %llx %llx",ctx.Dr0,ctx.Dr1,ctx.Dr2,ctx.Dr3);
    check(!(ctx.Dr0|ctx.Dr1|ctx.Dr2|ctx.Dr3),"debug registers DR0-DR3",got,"0 0 0 0");
}

/* gs points at the TEB; both ways of finding it must agree. */
static void teb_access(void) {
    void *self,*peb;char got[48];
    __asm__("movq %%gs:0x30,%0" : "=r"(self));__asm__("movq %%gs:0x60,%0" : "=r"(peb));
    snprintf(got,sizeof got,"%s",self==NtCurrentTeb() ? "same" : "different");check(self==NtCurrentTeb(),"gs:[0x30] is the TEB",got,"same");
    snprintf(got,sizeof got,"%s",peb==NtCurrentTeb()->ProcessEnvironmentBlock ? "same" : "different");check(peb==NtCurrentTeb()->ProcessEnvironmentBlock,"gs:[0x60] is the PEB",got,"same");
}

/* System calls made directly, as the protection does: "syscall" with Windows' numbers. */
NTSTATUS NTAPI NtWriteFile(HANDLE,HANDLE,PVOID,PVOID,IO_STATUS_BLOCK *,const void *,ULONG,LARGE_INTEGER *,ULONG *);
NTSTATUS direct_syscall_fff(void);
NTSTATUS direct_ntwritefile(HANDLE,HANDLE,PVOID,PVOID,IO_STATUS_BLOCK *,const void *,ULONG,LARGE_INTEGER *,ULONG *);
__asm__(".globl direct_syscall_fff\ndirect_syscall_fff:\n\tmovq %rcx,%r10\n\tmovl $0xfff,%eax\n\tsyscall\n\tret\n"
        ".globl direct_ntwritefile\ndirect_ntwritefile:\n\tmovq %rcx,%r10\n\tmovl $0x8,%eax\n\tsyscall\n\tret\n");
static void system_calls(void) {
    char got[32];IO_STATUS_BLOCK io;LARGE_INTEGER offset={0};BYTE buffer[8]={0};
    NTSTATUS s=direct_syscall_fff();snprintf(got,sizeof got,"%08lx",s);
    check(s==(NTSTATUS)0xc000001c,"direct syscall 0xfff",got,"c000001c");
    /* The protection's probe: a process handle, and a Key pointer no caller may pass. Windows
     * is believed to check the caller's pointers first; unconfirmed until run on Windows. */
    s=NtWriteFile(GetCurrentProcess(),NULL,NULL,NULL,&io,buffer,sizeof buffer,&offset,(ULONG *)~(ULONG_PTR)0);
    snprintf(got,sizeof got,"%08lx",s);check(s==(NTSTATUS)0xc0000005,"NtWriteFile(process, Key=-1) via ntdll",got,"c0000005 (unconfirmed)");
    s=direct_ntwritefile(GetCurrentProcess(),NULL,NULL,NULL,&io,buffer,sizeof buffer,&offset,(ULONG *)~(ULONG_PTR)0);
    snprintf(got,sizeof got,"%08lx",s);check(s==(NTSTATUS)0xc0000005,"NtWriteFile(process, Key=-1) direct",got,"c0000005 (unconfirmed)");
}

int main(void) {
    AddVectoredExceptionHandler(1,handler);
    puts("== CPU identity (Rosetta)");cpu_identity();
    puts("== CPU behaviour");nt_flag();
    puts("== Process state");process_state();
    puts("== TEB access");teb_access();
    puts("== System calls");system_calls();
    printf("== %d difference%s from Windows\n",failures,failures==1?"":"s");
    return failures!=0;
}
