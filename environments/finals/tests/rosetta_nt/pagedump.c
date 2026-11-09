/* pagedump.c — read-only JIT-page dumper for a live (possibly hung) process.
 * Same mach_vm_read shape as jitdump.c; never writes to or stops the target.
 *   clang -arch arm64 -O2 -o pagedump pagedump.c
 *   ./pagedump <pid> <addr-hex> <len-hex> [out.bin]
 * Dumps [addr, addr+len) to out.bin (default /tmp/rosetta-re/page-<pid>-<addr>.bin)
 * and prints the wrap_raw.py command to disassemble it. */
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <pid> <addr-hex> <len-hex> [out]\n", argv[0]); return 2; }
    int pid = atoi(argv[1]);
    mach_vm_address_t addr = strtoull(argv[2], NULL, 16);
    mach_vm_size_t len = strtoull(argv[3], NULL, 16);
    char path[256];
    if (argc > 4) snprintf(path, sizeof path, "%s", argv[4]);
    else snprintf(path, sizeof path, "/tmp/rosetta-re/page-%d-%llx.bin", pid, (unsigned long long)addr);

    mach_port_t task;
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "task_for_pid: %d (sudo?)\n", kr); return 1; }

    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); return 1; }
    mach_vm_size_t done = 0;
    while (done < len) {   /* 4K chunks so unmapped holes end the dump cleanly */
        mach_vm_size_t chunk = 0x1000, got = 0;
        if (len - done < chunk) chunk = len - done;
        static unsigned char buf[0x1000];
        kr = mach_vm_read_overwrite(task, addr + done, chunk, (mach_vm_address_t)buf, &got);
        if (kr != KERN_SUCCESS) { fprintf(stderr, "read @%llx: %d (stopping after %llx bytes)\n",
                                          (unsigned long long)(addr + done), kr, (unsigned long long)done); break; }
        fwrite(buf, 1, got, f);
        done += got;
    }
    fclose(f);
    printf("%s: %llx bytes from %llx\nwrap: python3 wrap_raw.py %s %llx\n",
           path, (unsigned long long)done, (unsigned long long)addr, path, (unsigned long long)addr);
    return 0;
}
