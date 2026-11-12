# Why THE FINALS still shows E111000D with the NT hook installed

Debugging brief, 2026-11-01. Runtime macfinals-7 / MacFinals-11.app. The game still stops
with "E111000D at 0202… Conflicting software detected!", the same error as before the hook.

## Status after r13 (read this first)

Follow-up work is in `R13_STATUS.md`. Where each cause below stands:

| Cause | Status |
|---|---|
| A. slot table full | **Confirmed and fixed** (64 reusable slots, 0 words dropped) |
| B. AOT not really off | **Ruled out**: anchors are seen in real processes |
| C. unseen patterns | **Open**: game launcher had 3 iret anchors, 0 fixed, 3 missed; 4 of 16 gathers started but never completed |
| D. check 4 inside Wine | **Passes** in the probe |
| E. hard-coded iretq address | **Open, top suspect** |
| F. skipped gates | Probe done (4/4 against Windows); Proton three-way not rerun |

Correction: the counters count *translations*, not executions, so "should show thousands"
below was wrong. Small counts are normal. The conclusion (the matcher was deaf) still held.

r13 also found a bug this brief missed: the payload used **SIMD registers the hook does not
save** (built at `-O2`). That crashed Steam and could corrupt guest XMM state. It is fixed by
`-mgeneral-regs-only`, and `mkpayload.py` now refuses any payload that uses them.

The original brief follows, unchanged.

## The main finding (measured, not a guess)

**In real processes, the hook's fixups have never fired once.** Every exit dump in
`/tmp/rosetta-re/hook-*.log`, about 370 process exits including the game, reads:

```
gather 0  scatter 0  iret fixups 0
```

That can't be right for a working hook. Wine's own syscall dispatcher runs
`pushfq`/`popfq`/`iretq` on every syscall (`dlls/ntdll/unix/signal_x86_64.c:3207–3454`), so
any hooked Wine process should show thousands of matches. The append hook is being called
(the rings fill to 131072 words), but the matcher never recognises anything.

So in the game, only the r9 converters are active:

| Check | ntstub (plain macOS test) | Real Wine / game |
|---|---|---|
| 1. POPF→PUSHF keeps NT | pass | **fail** (scatter/gather never fire) |
| 2. Exception context carries NT | pass | probably passes (r9) |
| 3. IRETQ with NT raises #GP | pass | **fail** (iret insert never fires) |
| 4. Injected NT survives resume | pass | **unverified** (see cause D) |

For the anti-cheat this looks almost the same as having no hook, which is why the error
didn't change.

## Most likely causes, in order

**A. The 16-slot table fills up and the matcher goes deaf.** `payload_arm64.c:105` has
`NSLOT 16`. Slots are keyed by buffer-object address and never released. ntstub runs its
trigger early in a fresh process with few buffers, so it works there. A real Wine process
creates many buffers across many threads. Once 16 are taken, line 282
(`table full: observed above, never matched`) applies to every later word, and nothing
counts those misses. This fits the data best.

**B. AOT may not really be off.** `hooklib.c:424` only checks that `DISABLE_AOT` is set in
the environment, not that Rosetta obeys it. Wine's dispatcher lives in `ntdll.so`, a dyld
image Rosetta can serve from the AOT cache, and AOT code never passes through the 0x18a0
helper. Upstream rosettax87_jit patches Rosetta's SIP check to unlock its hidden env flags,
and its loader writes the disable-AOT byte into memory instead of relying on the env var.
Both suggest the env var alone may not work. This can't explain the game's own code, which
is mapped by Wine and always JIT-translated, but it would explain the zeros in Wine's code.

**C. Real code emits patterns the matcher hasn't seen.** The matcher was learned from one
clang-built trigger. Real code may use other push addressing, 16-bit `pushf`/`popf`
(`66 9c`/`66 9d`), or flags built differently after other instructions.

**D. Check 4 inside Wine has never been tested.** ntstub's resume goes through macOS
sigreturn, which uses the patched decompose. Inside Wine, `NtContinue` resumes through
Wine's own `iretq` path, which goes through Rosetta's iret handler (`runtime+0x13848`). That
handler may convert flags its own way and drop NT.

**E. The IRETQ fault address is still hard-coded.** NOTES.md calls this "THE ONE UNSOLVED
PIECE": `sub x22,x1,#2` is only right when the return target sits right after the `iretq`.
At real `iretq` sites the #GP would report the wrong address.

**F. The ledger's own gates were skipped.** The NOTES.md "Remaining" list required an
nt_native rerun, the general-case delta and the three-way probe before integration.
r11/r12 went straight from ntstub to Steam and the game.

## What to do, in order

1. **Stop testing with the game.** Use `macfinals_probe.exe` under the hooked runtime. It
   runs the same NT checks through real Wine and can be compared with Proton on
   `ssh ubuntu`. Each run takes seconds, and it is the gate that was skipped.
2. **Add a diagnostic build to tell A, B and C apart.** Add counters for "table full",
   "slots claimed", "`movz #0x202` anchor seen" and "`msr NZCV` seen", and dump the ring to
   a file at exit. Run the probe once:
   - Anchors seen with a high "table full" count → A. Release or evict slots (LRU, or
     per-thread state instead of a fixed table).
   - Anchors seen but no matches and no table-full → C. Widen the matcher, using the dumped
     ring as the new test case for `host_payload_test`.
   - Anchors never seen while Wine's dispatcher is running → B. **Ask Rasmus before writing
     the disable-AOT byte in memory.** That goes beyond the green-lit scope (POPF/PUSHF/IRET
     flag handling only).
3. **Make the hook complain when it isn't working.** If a process exits with zero matches
   after heavy translation, log `hook installed but never matched` in the game log. That
   would have caught this on the first launch.
4. **Watch for a crash hazard once the matcher fires.** Wine clears NT with `andq`+`popfq`
   right before its own `iretq`. If the scatter fixup misses that `popfq` while the iret
   insert fires, a stale NT bit would make Wine's own `iretq` raise #GP.
5. **Fix the general-case IRETQ address (E).** Then the probe has to pass 4/4 on MacFinals
   and match Proton exactly, including check 4 through `NtContinue` (D).
6. Then Crab Champions (the regression check), then THE FINALS.
7. **If the probe matches Proton and the game still fails:** the "runs with a translator
   hook" result came from a full stack that also included Wine patches we don't
   have, first among them the GS/TLS one. That is the next suspect. Implement it ourselves,
   from what it's for.

## Do not

- Analyse the anti-cheat image to find out which check fails (CLAUDE.md boundary #3). The
  probe is how we tell.
- Treat ntstub 4/4 as proof the game is fixed. It proves the mechanism in a plain macOS
  process, not in Wine.
