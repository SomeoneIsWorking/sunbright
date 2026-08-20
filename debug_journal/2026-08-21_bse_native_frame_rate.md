# BetterSunshineEngine native frame-rate integration (2026-08-21)

The previous Eclipse assessment in this file was wrong in two material ways: it treated the native
game-rate path as unwanted because interpolation exists, and it said no source was available to
port. BetterSunshineEngine's FPS implementation is public source, and the user explicitly removed
Eclipse/Kuribo from scope on 2026-08-21. The correct target is to port BSE's timing behavior directly
into Sunbright's standalone recomp runtime without downloading or running Eclipse.

Source specification reviewed locally: BetterSunshineEngine's
[`src/patches/fps.cpp`](https://github.com/JoshuaMKW/BetterSunshineEngine/blob/69baa4f15bfb2980670cbde638fc22f97a394385/src/patches/fps.cpp)
at commit `69baa4f15bfb2980670cbde638fc22f97a394385`. The adapted subsystem retains
its GPL-3.0 provenance in `res/LICENSES/` and SPDX source headers.

## Root cause

Sunbright's first Native 60 implementation changed only
`JDrama::TVideo::waitForRetrace` from two fields to one. That doubles game ticks, but leaves SMS's
animation constant at `0.5` and ModelGate's per-tick step at `0.01`. The simulation therefore runs
more often without converting its game-owned timing inputs. The double-speed result is a direct
consequence of that incomplete contract.

BSE's `updateFPS` changes all three values together:

| game rate | retrace count | `0x804167B8` | `0x80414904` |
|---|---:|---:|---:|
| 30 Hz | 2 | 0.5 | 0.01 |
| 60 Hz | 1 | 1.0 | 0.02 |
| 120 Hz | 0 | 2.0 | 0.04 |

Native 60 uses BSE's exact 60 Hz row. Native Match Refresh retains Sunbright's one-field guest seam
and host pacing, while evaluating BSE's two numeric timing formulas continuously; a 144 Hz display
uses multiplier 4.8, animation rate 2.4, and ModelGate step 0.048. Match Refresh is paced, not an
unbounded loop. This is intentionally not BSE's 120 Hz `retraceCount=0` preset because Match Refresh
must cover arbitrary display rates rather than only BSE's three discrete settings.

## Ported ownership

- `sms-recomp/app/frame_rate.{h,cpp}` is the sole policy/formula owner.
- `sms-recomp/overrides/native_frame.cpp` writes the base timing trio immediately before the retail
  frame body.
- `sms-recomp/bse/frame_rate_fixes.cpp` scopes BSE behavior to its original call sites while keeping
  retail recompiled bodies alive: boid travel, AnimalBird/Boss Eel fixed animation delta,
  TJointCoin/Sand Bird rate, textbox entry duration, and `HX_MotionUpdate`.
- `sms-recomp/bse/frame_rate_logic.{h,cpp}` is the pure HX integrator used by both production and its
  CPU close-test, so the test does not duplicate the formula.

The function-entry override seam cannot faithfully express BSE's remaining discrete HX timer and
frame-rate initializer patches, which replace individual instructions inside several functions.
The proper fix is an instruction-level static-recompiler patchpoint facility or verified full ports
of every owning function. Watching global timer values and rewriting recognized constants would be
a magic-value bandaid, so it is not used. This is the only known BSE FPS-source gap in this
milestone.

Sunbright's existing widescreen system remains authoritative:
`sms-recomp/overrides/widescreen.cpp`, `hud.cpp`, and `widescreen_effects.cpp` already own native
projection, HUD anchoring, EFB-copy, and full-screen effect behavior. Importing BSE's guest
widescreen patches would create a second policy owner and regress that coverage.

## Verification

- CPU tests cover the 30/60/120/144 timing formulas and both branches of the production HX motion
  integrator, including rejection of an invalid multiplier.
- A windowless Native 60 run reached Delfino gameplay, exercised the BSE overrides, exited cleanly,
  and produced zero amdgpu timeout/reset events.
- A windowless Native Match Refresh run with `SBR_DISPLAY_HZ=144` reached the same gameplay path,
  applied the 144 Hz policy, exited cleanly, and produced zero amdgpu timeout/reset events. The
  safety harness caps GPU submissions at 60 Hz, so this proves game-policy/runtime execution, not
  visible 144 Hz presentation.

The falsifier for the base timing result is a controlled native-rate run showing that the three BSE
inputs are not present at the frame boundary or that gameplay advances faster at 60 Hz than at
30 Hz over equal wall-clock intervals. The current headless boot checks establish wiring and health;
they do not yet constitute that motion-duration comparison.
