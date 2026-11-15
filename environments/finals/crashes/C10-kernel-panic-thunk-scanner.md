# C10: macOS kernel panic from the r20 thunk scanner

| | |
|---|---|
| First seen | 2026-11-07 18:20 and 18:21 (two forced reboots) |
| Origin | ours (r20 finished-thunk scanner in the hook) |
| Status | mitigated: quarantined behind `MACFINALS_THUNK_PATCH=1`; the cause in the kernel isn't fixed |
| Reproducer | running the scanner (don't) |
| Fix | – (the method constraint in `CLAUDE.md` exists because of this) |

## What you see

The Mac panics and reboots.

## Evidence

Panicked task: the hooked process. Faulting user frame: `scan_patch_thunks+494`, a
`mach_vm_read_overwrite` of a translated anonymous executable region. Kernel data abort at
`far=0x14`. Panic files copied to `/tmp/rosetta-re/panics/`. Source: `tests/rosetta_nt/NOTES.md`,
"r20 (2026-11-07)".

## What we think

Reading a translated mapping with `mach_vm_read_overwrite` while translation is live (256K
chunks, from dyld-constructor time, alongside another mach client) hits a kernel bug.

## Rule that came out of it

No `mach_vm_*` copy or protection call on translated mappings until individually re-proven; read
translated memory with plain loads behind a SIGSEGV/SIGBUS recovery handler (`CLAUDE.md`, "Method
constraint").
