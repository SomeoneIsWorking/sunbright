# 7.0 million `getenv` calls per run — measuring diagnostic-gate sprawl instead of guessing at it

The trigger was a correction: *"you should use lucent for config, not getenv."* I had "fixed" a
per-primitive `std::getenv` in aurora's `draw_prim` by caching it in a static — which removed the
2.9 ms/frame but kept the banned `getenv + gated fprintf` idiom, treating the symptom. The real fix
is one channel-gated logger per callsite. Doing that properly meant finding out how much of this
codebase had the same defect, and *that* could not be read off the source.

## Why it had to be counted

`grep -c getenv` gives ~120 distinct env vars in aurora alone, and the count is meaningless for
sizing the work: the same source line costs nothing inside an `if (!s_init)` block and costs
millions of environ scans inside `draw_prim`. This is the failure CLAUDE.md names — *a grep count is
text, not code* — and the previous entry in this journal records a 0-for-3 record on naming hot code
by reading it.

So: `tools/perf/count_getenv.c`, an `LD_PRELOAD` interposer that counts `getenv` by name.

Two design points, both from the negative-design rule:

* **It cannot report an empty table.** If it intercepted nothing it prints `INTERPOSER SAW NOTHING`
  and says explicitly that this is *not* evidence the program makes no calls — a failed `LD_PRELOAD`
  and a clean program are otherwise the same output.
* **It does not rely on its destructor.** The first two runs produced no report at all: `sms-boot`
  exits through a path that does not run `__attribute__((destructor))`, and an un-run destructor is
  indistinguishable from "no calls were made". It now flushes every 200k calls as well.

Validated against a known-positive first (a program calling `getenv("FOO")` twice and `"BAR")` once
→ reported exactly `2 FOO`, `1 BAR`) before being pointed at the game.

## What it found

30 s Delfino run, `SB_HEADLESS=1 SB_TURBO=1 SB_STAGE=1`:

    getenv calls intercepted: 7000000 total across 512 distinct names
       1677795  SB_SKIP_MIRROR_FAR
       1677795  SB_SKIP_MIRROR_NEAR
       1677795  SB_SKIP_MIRROR_DBG
        296819  SB_DBHEAD_DBG
        141419  SB_TEV_DBG
        114539  SB_CLOUD_DBG / SB_XH_MIRROR_DBG / SB_XH_DBG / SB_TEXOBJ_DBG  (each)
         83720  SB_TEVORDER_DBG
         ...

Three switches from a **closed** investigation — the title sky crosshatch bisection — accounted for
**5.03M of the 7.0M**, scanning the environment three times per sky draw for a diagnostic nobody has
enabled in weeks.

## The rule applied, and it is not "put everything on the logger"

CLAUDE.md splits these two ways, and the split is what makes the conversion safe:

* **Logging gates → a channel.** aurora gets `lucent::Channel` (hoisted `static const`, a relaxed
  atomic load); `decomp/sms` and `sms-boot` get the project's own `SB_LOG` registry, which already
  existed and already has a commit gate. Both are "one logger, one call site".
* **Behaviour toggles stay env vars** — `SB_SKIP_MIRROR_FAR/NEAR` change what is drawn, so they are
  not logging and do not belong on a channel. They were hoisted into statics instead. *The cost was
  never the env var; it was the uncached lookup.*

`SB_LOG_ON(chan)` was added to `sb_log.h` for the case a diagnostic must do real work before it can
print (resolve a name, walk a list, take a backtrace) — `SB_LOGC`'s arguments are evaluated before
its own gate can help, so those sites need a cached predicate, not a cached print.

## Result, measured the same way each round

| round | total | ours (`SB_*`/`AURORA_*`) |
|---|---|---|
| before | 7,000,000 | ~6.97M |
| after mirror bisect | 1,400,000 | 699,513 |
| after decomp batch | 800,000 | 232,659 |
| after drawflag/copydbg | 600,000 | 190,946 |
| after dbheadpkt | **400,000** | **35,392** |

**198× reduction on our own switches.** The remaining 35k is per-frame and per-pipeline (≈2k each)
and is not worth further churn; the ~365k balance is SDL/Dawn/locale internals, not ours. Stopping
here is a choice, and it is recorded rather than left to look like completeness.

Each round surfaced a new top entry that the previous round had masked — which is the argument for
re-measuring after every change instead of converting the first list top-to-bottom.

## What it is worth in frame time — smaller than it sounds, and stated as such

The temptation here is to present a 198x reduction as a performance result. It is not one, and the
honest number matters more than the impressive one.

A wall-clock A/B was not possible: this machine carried load average 9.8 during the attempt, and the
journal entry before this one already records that frame times taken at different loads are not
comparable. So the effect was derived from two quantities that ARE load-robust — the count of calls
removed, and the cost of one call measured in CPU time (`tools/perf/getenv_cost.c`,
`CLOCK_PROCESS_CPUTIME_ID`):

    environ entries: 116
    present name : 80.8 ns/call
    absent  name : 96.5 ns/call   <- what a switched-off diagnostic pays

The benchmark carries its own control: an ABSENT name must come out slower than a present one,
because absent is the full scan. If they matched, the benchmark would not be measuring the scan at
all, and it says so and refuses the numbers.

    6,600,000 calls removed x 96.5 ns = 0.64 s of a 30 s run
                                      ~ 468 us of a ~22,000 us frame   = 2.1%

**2.1%.** Real, worth having, and nowhere near a 60fps lever — that remains the per-primitive path
at 45,914 `draw_prim` calls per frame, ~340 ns each, as established in
`2026-08-05_runtime_cost_comparison_for_60fps.md`. This change was a correctness and hygiene fix
that the user asked for; the perf is a side effect, and inflating it would be exactly the kind of
number this journal exists to prevent.

## Two bugs the conversion exposed, neither of which was the point

**1. A diagnostic that SIGSEGVs the moment you enable it.** `SB_LOG=entrymat` exited 139.
`sb_b76_material` is declared `__attribute__((weak))` and is defined only inside `sms-boot`, so when
it is not linked the symbol resolves to address 0 and the unguarded call jumps to 0. The file
carried a comment asserting this was *"safe because every call site below is gated behind a getenv()
debug flag, never hit in a normal test run"* — the gate is not what makes it safe, and the claim had
survived because **the switch had never once been turned on**. Two more sites called
`sb_boot_capture_phase()` the same way. All now test the address, and the comment says why.

**2. Channels whose gate and print disagreed.** The mechanical `fprintf → sb_logf` pass derived the
channel name from the print's `[tag]`, so `entrymat` gated on one name and printed on `entry-mat`,
and `dbheadpkt` printed on `dbhead`. Enabling either gave silence. Caught only because every
converted channel was run and its output counted — not because the code looked right.

That verification also found three channels that print nothing on Delfino for a legitimate reason
(their inner predicate targets a file-select material). Silence there is indistinguishable from a
broken instrument, so each now announces itself when enabled:

    [dbheadpkt] ARMED: scanning every flushed buffer for a packet whose material low-24 bits are
    0xc97c48 (the sea-mask material). No output after this line means that material was never
    flushed in this scene, not that the scan did not run.

## Verification

* Every converted channel run individually and its line count recorded: `texobj` 32467, `matanm`
  4608, `tevname` 1027, `tevorder` 400, `drawflag` 400, `dbhead` 219, `death` 106, `cloudtex` 50,
  `tevstage` 12, `jkr` 9, `params` 3, plus the three ARMED/INACTIVE announcers. lucent side:
  `pipeblend` 1824, `lensuv` 440 (on the title, where LensFlare actually draws), `texresolve` 131,
  `copybind` 50 — and **silent with the channel off**, checked in both directions.
* `SB_LOG=list` registers all new channels.
* No render regression: Delfino mean RGB (135.2, 144.6, 145.9) against the recorded (135.2, 144.6,
  145.8); title at the same checkpoint the previous entry used (187.4, 216.9, 234.2) against
  (187.2, 217.0, 234.7).
* `tools/diag_registry.py check` green; the generated `docs/diagnostics.md` loses 25 switches net.

## The transferable part

The instrument took ten minutes to write and answered in one run a question that reading 120 call
sites would have answered wrongly — the three switches that dominated the total are in a file I had
already read past twice while looking for hot `getenv`s, because nothing about the source says how
often that line runs. **Frequency is a runtime property; it has to be measured at runtime.**

And the second-order finding is worth more than the perf: switching a diagnostic ON for the first
time crashed the process and exposed two channels that could never have printed. Diagnostics that
are never exercised rot exactly like untested code, and the sweep that exercises them all at once is
how you find out.
