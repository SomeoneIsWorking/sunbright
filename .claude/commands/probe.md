# Probe / drive the running game (interactive)

Drive and inspect the RUNNING Sunbright PC port **headlessly** over its HTTP probe REPL —
send inputs, take screenshots, read render/guest state — without the GUI and without
relaunching between actions. Use this instead of ad-hoc one-shot `./run.sh`/dump runs.

⚠ **This skill + `tools/gp` are a LIVING system — EXPAND them whenever you add an endpoint,
a `gp` subcommand, or a new compare/diagnosis mode.** Proper interactive tooling (esp. live
render-flow comparison vs the oracle) is a first-class goal, not a side quest. Keep `tools/gp`
and this file in sync; this file is the durable memory of the probe surface.

## ⚡ Fastest loop: `tools/gp scene` (ONE command — no manual kill/launch/wait dance)
The renderer-fidelity workflow in a single command. It kills any stale instance, launches ngx
headless with AUTOSTART, **drives to real gameplay** (waits for the HUD to draw — not a random
title/transition frame), freezes the aligned GX-vs-ngx A/B, captures `/abshot2`, and prints the
per-region pixel delta:
```bash
tools/gp scene                 # Delfino-plaza gameplay A/B baseline (the number to drive down)
tools/gp scene TITLE           # freeze at the title logo instead (multi-matrix logo case)
tools/gp png                   # convert ab2.{gx,ngx}.ppm -> .png to LOOK at them (Read tool)
```
Then iterate on the FROZEN frame with zero re-launch (gx oracle held, ngx re-renders each toggle):
```bash
tools/gp only 12   ; tools/gp ab2     # render ONLY tev_index 12 + re-diff
tools/gp skip 12   ; tools/gp ab2     # SKIP it
tools/gp noblend 0 ; tools/gp ab2     # force every material opaque
tools/gp dbg tex   ; tools/gp ab2     # shader debug mode (tex|ras|normal|uv0|…)
tools/gp pixbatch 0.5 0.45            # which captured layers cover an NDC pixel (combiner/blend/tex)
tools/gp pixbatch -901 12             # x=-901 => full BATCH CLIP DUMP for tev_index 12: PE block +
                                      #   per-stage ce/ae/map/coord/chan/kc/ka + rswap/tswap + the 4
                                      #   decoded TEV swap tables (id->RGBA swizzle) + texmaps + UV bbox
tools/gp freeze 0                     # release (game keeps running)
```
⚠ `gp scene`'s `waitgameplay` polls the FLAKY `hud_quads` signal and may never fire even when the
scene is fine. If it hangs, the instance is still up — just `curl /ngxfreeze?on=1` then `curl
/abshot2` directly (frame_swaps climbing in `/ngxshape` = scene ready).
The `/abshot2` capture is ZERO-DRIFT (Dolphin GX XFB + ngx native render from the SAME present),
so the delta moves only for ngx changes. `tools/gp wait [swaps]` / `waitgameplay` poll readiness.
⚠ `SUNBRIGHT_STATE=<save>` deterministic load is KNOWN-BROKEN on the native path (vi_end_field_event
stops firing → never reaches the threshold; and State::LoadAs deadlocks the SDL thread). Use AUTOSTART.

## Differential render harness: ngx vs the gx oracle (the key capability)
Run BOTH renderers concurrently and diff them LIVE to tell real native-renderer bugs from
faithful rendering (animations, real game state):
```bash
tools/gp launch both      # ngx native (port 17654) + gx-oracle = recomp Dolphin-GX (17655, isolated)
tools/gp pad start 200    # drives BOTH in lockstep (same recomp logic, only the RENDERER differs)
tools/gp compare logo     # screenshots both -> logo.ngx.png / logo.gx.png + logo.diff.png (side-by-side)
                          #   prints meanAbsDiff: ~0 = ngx matches oracle (faithful); large = real bug
```
The gx oracle is recomp + `SUNBRIGHT_NGX_PRESENT=0` (Dolphin's GX render of the SAME game state).
⚠ GOTCHA (burned once): `SUNBRIGHT_NGX_PRESENT`/`NGX_SHAPE` are honored by VALUE now (`=0` disables);
they used to test mere presence, so `=0` still enabled NGX → BOTH instances ran NGX and "compared"
identical. Verify the oracle is real: only `scratch/logs/gp.ngx.log` should say "NATIVE PRESENT
enabled". (This was how the title-logo "tearing" was first mis-called faithful — it's a real ngx bug;
the oracle renders the logo clean.) For pixel-exact compares, load the SAME save state in both.

## The tool: `tools/gp` (use this first)
A thin CLI over the probe REPL. The game is launched DETACHED, so it stays up across many
separate `gp` calls (the interactive loop = launch once, then drive).

```bash
tools/gp launch                  # boot headless: native renderer (NGX present) + probe, detached
tools/gp pad start 200           # press Start 200ms — THIS is how you skip the THP/FMV + advance menus
tools/gp shot title              # screenshot -> scratch/screenshots/title.png (+ prints mean brightness)
tools/gp ngx                     # /ngxshape: capture + render stats (materials, lighting, batches…)
tools/gp present                 # /ngxpresentlive: native-present renderer stats (frames, pipelines)
tools/gp metrics                 # perf JSON (speed/FPS/VPS)
tools/gp drawstats               # Dolphin's per-frame GX draw/prim counts (A/B vs ngx)
tools/gp r 8040e190 8            # read 8 guest words at 0x8040e190
tools/gp fn 802b5b30             # nearest function name for an address
tools/gp get '/ngxshape'         # raw curl any endpoint
tools/gp status | kill           # is it up / stop it
tools/gp launch SUNBRIGHT_FASTBOOT=1     # extra env overrides (fastboot, STATE=…, NGX_PRESENT=0, TEVDBG=…)
```

Navigation cheatsheet (combo = `a b x y z start l r up down left right`, joined with `+`):
- **Skip the THP/intro movie**: `gp pad start 200` (repeat through the attract sequence). Do NOT
  fastboot to reach the title-logo / file-select — those are real-time 3D and are exactly where
  the native renderer's bugs show. FASTBOOT jumps straight to Delfino gameplay (skips them).
- Title logo → file-select: `gp pad start 200`. File-select → game: walk with `up`/`a`.
- Pad input reaches the game via Dolphin's input override, so it works headless + headed.

## Render-path env (pass to `gp launch`)
- `SUNBRIGHT_NGX_PRESENT=1` (gp default): the native PC renderer's frame is the on-screen image.
  `=0` → Dolphin-GX baseline (for A/B). `SUNBRIGHT_NGX_SHAPE=1` = capture only (no present).
- `SUNBRIGHT_NGX_TEVDBG=tex|ras|cat|bid` — debug shader output: raw texture0 / vColor(col0) /
  material category color / tev_index encoded as RGB (read a pixel → which material).
- `SUNBRIGHT_NGX_NOLIGHT=1` — bypass native lighting (A/B).
- `SUNBRIGHT_DBG_RASCOLOR=1` / `SUNBRIGHT_DBG_TEXCOLOR=1` — **Dolphin-side** (fork PixelShaderGen):
  a recomp-GX run renders GX's true col0 / texture sample, for ground-truth A/B vs ngx.
  ⚠ Dolphin caches compiled shaders on disk (`<home>/.cache/dolphin-emu/Shaders`) keyed by UID, which
  bypasses generator edits — move that dir aside to test PixelShaderGen changes.

## Full REPL endpoint list (runtime/probe_server.cpp; `tools/gp help2` prints the live one)
`/r?a=&n=` reads · `/r16?` · `/w?` poke · `/gx` CP/FIFO · `/cur` cur thread · `/stack?sp=` ·
`/fn?a=` addr→name · `/metrics` perf · `/pad?do=&ms=` input · `/screenshot?name=` PNG ·
`/ngxshape` capture+render stats · `/ngxpresentlive` present stats · `/ngxrender` offline render+PPM ·
`/tevshader` TEV→GLSL self-test (+ dumps live material GLSL) · `/drawstats` Dolphin GX counts ·
`/j2d` HUD pane tree · `/jas` `/njas` `/vpb` audio · `/tracelog` `/trace?` `/nintr` `/drawsync` ·
`/tex` `/ngxvtx` `/ngxmesh` `/vkquad` self-tests · `/verify` 60fps midpoint · `/help`.

## Always
- ALWAYS headless (`gp launch` sets `SUNBRIGHT_HEADLESS=1`); never `./run.sh`/GUI (user owns the
  display). Never overlap two instances (`gp launch` kills the old one first).
- Screenshots land in `scratch/screenshots/`. Read them with the Read tool or PIL.
- Kill a stuck run: `tools/gp kill` (or `pgrep -x sunbright | xargs -r kill -9`).

## Self-update
When you add a probe endpoint or a `gp` subcommand, update BOTH `tools/gp` and this skill so the
next session knows it exists (this skill is the durable memory of the probe surface).
