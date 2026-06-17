# Native-renderer A/B harness (ngx vs Dolphin-GX), same game state

The only trustworthy way to judge native-renderer (ngx) fidelity is **rendered pixels from the
same game state**, ngx vs an oracle. This doc is the canonical recipe + why it's valid.

## TL;DR — use `tools/gpshot`
```
tools/gpshot                 # FASTBOOT -> Delfino gameplay, zero-drift GX-vs-ngx A/B + region delta
tools/gpshot --fs            # AUTOSTART -> file-select (the shredded skinned-Mario case)
tools/gpshot --fs '/ngxskip?ti=10'   # apply probe cmds before the capture (isolate a material)
```
Output: `scratch/screenshots/ab2.gx.png` (oracle) + `ab2.ngx.png` (native) + a per-region mean
pixel delta. Both PNGs are the **same present**.

## Why `/abshot2` is a VALID same-state comparison (not a desync confound)
`/abshot2` arms a dual capture in `Present.cpp::ProcessFrameDumping`: in ONE present it writes the
Dolphin GX XFB (`ab2.gx.ppm`) and the native ngx texture (`ab2.ngx.ppm`).

1. **Single core.** One recomp CPU produces the frame; both renderers consume the SAME frame. A
   single core cannot drift from itself — this is strictly better than syncing two processes (the
   old `gp launch both` + `pad start` method drifted in animation phase = the "wash confound").
2. **Untainted oracle (code-proven).** The ngx capture is pure-observer: `J3DShape::draw`
   (0x802e0390) runs the **real draw first** then observes; every GX-state override
   (`gxloadposmtximm`, `gxloadposmtxindx`, `gxsetchanctrl`, `gxsetchanmatcolor`, `gxsetchanambcolor`,
   …) captures for ngx and then **super-calls the real recomp function** (`o(cpu)`). So Dolphin's GX
   command stream is identical to a pure run; `ab2.gx.ppm` == what vanilla Dolphin-GX would render.
3. **Atomic same present.** Both PPMs are written in the same `ProcessFrameDumping` call.
4. **Self-certifying liveness.** `/abshot2` reports `ngx_frame=N` (the published ngx snapshot id).
   Two successive live captures should show N advancing; if N is stuck, the snapshot is stale (a
   no-3D frame kept the last buffer) — distrust that A/B.

`/ngxfreeze?on=1` latches the snapshot so you can flip debug modes/isolation on the SAME geometry
across many probe calls (it holds the GX XFB captured at freeze + re-renders ngx from the frozen
J3D snapshot).

### What abshot2 does NOT answer
The GX oracle renders **our recomp's** GX stream. So abshot2 tests *ngx renderer vs Dolphin-GX of
the same recomp output*. It does NOT tell you whether the **recomp** computed the right geometry in
the first place. If `ab2.gx.ppm` ALSO shows the defect (e.g. shredded Mario), the bug is in the
recomp (game logic), not the renderer — switch to a recomp-vs-JIT comparison
(`SUNBRIGHT_DISABLE_RECOMP` oracle / `SUNBRIGHT_DIFF`).

## Sandboxed-Bash gotchas (these broke every naive launch attempt)
The game binary + probe HTTP both need network/GPU, which the Claude Bash **sandbox blocks
silently** (curl returns empty, launch exits 1 with NO output). Rules that actually work:
- Run launches/curls with the Bash sandbox **disabled** (`dangerouslyDisableSandbox`).
- The game must run in the **foreground** of the call — launching it with `&` aborts the call.
  `gpshot` runs the game foreground and puts the *capturer* (poll → freeze → abshot2 → kill) in a
  background subshell.
- **Never** `pkill -f "build/sunbright"` — that substring matches the driving shell and kills the
  whole call. Kill ONLY by exact name: `pkill -x sunbright`.
- A new foreground Bash call **kills a prior `run_in_background` game task**, so do launch + wait +
  capture all in ONE call (that's what `gpshot` is).
- Stale instances squat probe port 17654 and serve the OLD binary — `gpshot` pkills first.

## Two-process / save-state sync (the "both cores" ask) — status
A genuinely independent second core synced via save-state is the canonical "two cores, same state"
tool, but on the native path it is currently blocked:
- `State::LoadAs` from the SDL thread deadlocks against the governor-parked CPU thread; the in-code
  fix is to run the load on the CPU thread (but `Core::RunOnCPUThread` jobs aren't serviced by our
  recomp loop — it doesn't poll the CPU job queue).
- A Dolphin save-state does NOT contain our recomp execution state (native call stack / nthr
  scheduler), so a loaded state can't be cleanly resumed to render a frame.
For render fidelity, `/abshot2` (single core) is the correct and superior mechanism. A two-process
save-state sync's real value is recomp-vs-JIT *game-state* comparison, not renderer comparison.
