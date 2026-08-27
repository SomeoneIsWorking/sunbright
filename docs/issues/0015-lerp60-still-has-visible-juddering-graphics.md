---
id: 15
title: Lerp60 still has visible juddering graphics
status: investigating
symptom: user observes graphics stepping or juddering in interpolated 60 FPS despite existing per-draw coverage reports
tags: reported,60fps,interpolation,judder,graphics
created: 2026-08-26
updated: 2026-08-28
---

## Observation

The running product still has visibly juddering graphics in `Interpolated 60 FPS`. This falsifies
using high aggregate tag or population coverage as proof that presentation is fully smooth. The
affected graphic or region is not yet identified.

## Investigation boundary

Compare `interpolated-60` with `native-60` over overlapping guest retrace labels while a scripted
C-stick produces sustained geometric motion. Measure consecutive presented RGBA frames, not draw
counts alone:

- whole-frame and cropped temporal step evenness;
- spatial alternation cells, with Native 60 as the scene-content control;
- duplicate presents and presentation timing separately from object-space snapping.

The comparison must include a forced-snap positive control. A region that alternates only in
lerp60 is a missing interpolation treatment; alternation shared by Native 60 is scene content; a
globally uneven or duplicated lerp60 series points to presentation cadence rather than a target.

## Instrument correction (2026-08-27)

The first comparison is invalid and must not be used for attribution. Native 60 produced 33 frames
from the pre-change binary; lerp60 produced only 8 readable frames from the later binary before an
amdgpu illegal-command-stream reset. `compare_modes.py` accepted the unequal, partial series and
ranked a lower-right cell anyway. That output suggested the animated water gauge, but it did not
prove the gauge caused the reported judder.

The gauge and persistent J2D pane identities remain implementation candidates backed by their
guest draw semantics and live pairing counts. The replacement comparator now binds captures to the
same binary, tool revision, configuration, and complete GPU-clean manifests; requires an exact
Native60 repeatability control; enforces Native60 `main` and Lerp60 alternating `main`/`sub` role
cadence with the expected guest ticks; and refuses unequal samples, unequal guest-time spans,
changed frame bytes, or partial runs before printing a pixel result. Scripted input is exclusive,
so live keyboard/controller state cannot perturb a capture. Each run must also log the requested
effective renderer/frame-rate mode, deterministic virtual clock, exact input script, and exclusive
input policy; this prevents `.env` from silently replacing the comparator's requested contract.

No replacement capture has been attempted after the reset. The comparator remains a spatial
localizer for image-step unevenness, not a scanout-timing probe or an object-identity join. Issue 17
has now completed the replay-range validation gate. The next comparison must still produce a clean
three-run Native60 / Native60-repeat / Lerp60 set before any region is interpreted.

## Capture-contract audit (2026-08-27)

The first hardened version still could not publish a valid capture. It searched its log for an old
GPU-clean sentence that the current in-process watcher never emits, and retained replay samples
inherited the primary frame's `main-tN` dump label. It also bound only the host executable: a
different user ROM or guest DOL could therefore pass its same-binary check. `--width` described how
analysis would decode the bytes but did not force or bind the runtime height.

The contract now uses the guarded launcher's return code as the GPU-clean authority: any nonzero
watcher/game result refuses before a manifest is written. Fixed interpolated 60 labels the primary
emission `main-tN` and its retained sample `sub-tN`; the focused runtime test proves the pair shares
the same guest tick and rejects invalid replay coordinates. Capture manifests are schema 3 and bind
full ROM and DOL content hashes, the current host binary, the capture/analysis and guarded-launch
source set, exact 1280x960 RGBA byte counts, stable texture-resolution evidence, configuration, and
every frame hash. Texture-resolution records bind width, height, mip count, and format; timestamps
and ASLR-dependent host pointers are removed. Native60 repeatability requires the exact ordered
resolution-event hash. Native60 versus Lerp60 compares the canonical unique descriptor-set hash,
because retained emissions legitimately alter event order and duplicate counts. This detects
descriptor population and mip/format drift but does not independently hash texture contents; the ROM
and DOL hashes bind the guest assets. The source
revision also covers the guarded launcher and GPU-watcher implementation and must remain unchanged
for the duration of each capture.

`run.sh --diagnostic --isolated-environment` clears inherited or `.env` project diagnostic knobs,
then reapplies the comparator's validated explicit assignments after `.env`. A shell-level control
set `SUNBRIGHT_ROM` to a deliberately missing path in the protected assignment file while `.env`
contained the real ROM; `run-recomp.sh` refused the missing explicit path before building, proving
the explicit value won. The opposite GPU result is covered by `compare_modes.py --selftest`: return
code 86 cannot produce clean provenance while return code 0 can.

The final serialized GPU comparison completed with the following exact command shape:

```bash
uv run --frozen python tools/interp/compare_modes.py capture-native \
  --rom /path/to/rom.rvz --dol scratch/bin/sms.dol \
  --after 1820 --count 33 --timeout 180 --width 1280 --height 960 \
  --pad '800:CSTICK=100/0,1800:CSTICK=0/0,1820:CSTICK=20/0'
uv run --frozen python tools/interp/compare_modes.py capture-native-control \
  --rom /path/to/rom.rvz --dol scratch/bin/sms.dol \
  --after 1820 --count 33 --timeout 180 --width 1280 --height 960 \
  --pad '800:CSTICK=100/0,1800:CSTICK=0/0,1820:CSTICK=20/0'
uv run --frozen python tools/interp/compare_modes.py capture-lerp \
  --rom /path/to/rom.rvz --dol scratch/bin/sms.dol \
  --after 1820 --count 33 --timeout 240 --width 1280 --height 960 \
  --pad '800:CSTICK=100/0,1800:CSTICK=0/0,1820:CSTICK=20/0'
uv run --frozen python tools/interp/compare_modes.py analyze --width 1280 --height 960
```

The three captures must run without source, binary, launcher, asset, configuration, or input changes
between them. A successful analysis still identifies only screen regions. The highest-excess region
must be joined to draw identities inside the Lerp60 run before naming a missing interpolation target.
The camera-only script is deliberate: it rotates rapidly from guest tick 800, stops on the
ocean-facing view at tick 1800, then resumes a slow rotation at tick 1820 for the 1822..1854
capture window. This keeps water in view while proving real camera motion, without moving Mario into
an NPC, wall, or dialogue trigger. The exact script is bound into every manifest.

### First serialized attempt

The authorized Native60 leg stopped immediately with guarded exit 250 and zero dumps; neither later
leg ran. There was no kernel GPU fault. Dawn validation aborted submission 1 with the exact error:
`Texture "Resolved Texture" usage (CopyDst|TextureBinding|RenderAttachment) doesn't include
CopySrc`, while the flight report recorded `readbackQueuedThisSubmit=1` and 4,915,200 readback
bytes. Evidence:
`scratch/gpu_crash/session_3361042_18ce0c2af8275947_recomp-aurora.flight.report.txt` and
`scratch/logs/cadence_native.log`.

`SBR_SMOOTH=1` had enabled a separate every-frame comparison sink, which copied that resolved
texture to a buffer. This comparator does not read the smoothness sink, so carrying the switch was
a second, unrelated capture path and has been removed. Its self-test now asserts that constructed
capture commands do not arm `SBR_SMOOTH`. The Aurora texture-usage defect remains real for clients
that do request the sink and must not be misreported as a Lerp60 result. A new three-run set requires
the Aurora `CopySrc` fix to be integrated and built, followed by fresh serialized GPU authorization.

### Second serialized attempt

After the `CopySrc` fix and its controls passed, the recaptured Native60 leg completed with guarded
exit 0 and wrote all 33 exact `main-t1602` through `main-t1634` RGBA frames. The comparator still
refused it and wrote no successful manifest because its required texture manifest was empty;
Native60-repeat and Lerp60 did not run.

This was logger-contract drift, not evidence that the scene resolved no textures. Sunbright sets
Lucent's project prefix to `SBR_`, so Aurora's `texresolve` channel is enabled by
`SBR_LUCENT_DEBUG`; the launcher and comparator still used obsolete unprefixed `LUCENT_DEBUG`.
The capture owner now uses only `SBR_LUCENT_DEBUG`, and its self-test rejects the obsolete spelling.
The 33 frames from this attempt remain invalid evidence and must be replaced by a newly authorized
Native60 capture whose non-empty texture manifest passes the complete contract.

### Third serialized attempt

With the prefixed logger variable corrected, Native60 again exited cleanly and wrote all 33 frames,
but the launcher still published an empty manifest and the comparator again refused before either
later leg ran. The log contained more than 100 valid current-format records such as
`[timestamp] [texresolve] static 256x256 mips=1 fmt=1 data=0x...`; the stale parser required
`[texresolve]` at column zero.

The parser now accepts the exact current Lucent record shape and normalizes it to stable dimensions,
mip count, and format. Its controls prove that different timestamps and host pointers normalize
identically, a changed mip count remains different, and a wrong channel is ignored. These third-run
frames also remain invalid because no successful manifest was published.

### First complete three-run set

After the parser fix, Native60, Native60-repeat, and Lerp60 each completed with guarded exit 0,
33 exact 1280x960 frames, guest tick span 1602 through 1634, non-empty texture evidence, and a
published manifest. Native60 and its repeat matched all 33 frame hashes and had identical ordered
247-record texture-resolution streams. Lerp60's alternating `main/sub` role and shared-pair ticks
also passed.

Analysis correctly refused before scoring because the original cross-mode gate required the whole
ordered texture-event hashes to match. Lerp60 had 243 events in a different order, which is expected
when retained replay emissions exercise texture lookup on a different presentation schedule. The
canonical unique descriptor sets were exactly equal at 87 entries on both sides. The repaired gate
therefore keeps exact ordered equality for Native60 repeatability, but compares the unique descriptor
set across Native60 and Lerp60. Controls prove duplicate/reordered events preserve the set hash and a
changed mip count changes it. Because capture tooling is self-hashed, the repaired schema invalidates
this otherwise clean set and requires another complete three-run capture before analysis.

That earlier set also used forward movement together with camera rotation; Mario reached an
NPC/wall interaction and could open dialogue. The final replacement uses only
`400:CSTICK=100/0`, matching the then-established camera control in C039. Later inspection showed
that only the broken Lerp60 trajectory faced the sea at the capture window; Native60 faced a wall.
The schema-5 replacement therefore starts the same rotation at guest tick 800 on every mode.

### Rejected camera-only result

After the renderer extraction restored the structure gate, the final schema-3 Native60 and repeat
runs each completed with guarded exit 0, 33 exact frames, and guest tick span 1602 through 1634.
All frame hashes, provenance, and ordered texture-resolution events matched exactly. The first
camera-only Lerp60 attempt reached 750 simulation ticks plus 750 retained presentations and was
still advancing at 9.4 ticks/s when the 120-second wall cap expired just before the 1600-present
dump window. It produced no frames or manifest and was not used. Repeating only that unchanged leg
with a 180-second safety cap completed cleanly with the same 33-frame tick span and valid alternating
`main/sub` labels. All three runs resolved the same 84 unique texture descriptors; Native repeat
also retained the exact ordered-event hash, while Lerp's permitted ordered-event hash differed.

The capture contract itself passed, and the positive control proved that the spatial statistic can
report the opposite answer on the captured pixels. The cross-mode result is nevertheless invalid
for spatial attribution. Visual inspection of frames carrying the same guest-retrace label showed
Native60 and Lerp60 facing substantially different directions from the first captured frame onward.
The camera-only script is keyed to `PADRead` count, while Native60 and interpolated 60 advance game
simulation at different rates, so the same script and guest-retrace span do not establish the same
camera trajectory.

The strongest reported bottom-band cells were also not water: they covered the changing black
dialogue/text strip. The old analysis therefore compared different viewpoints and ranked a snapping
2D overlay. Its scalar scores and cell ranking must not be used to identify a water or world-space
lerp target.

The replacement gate must record the camera pose at the presentation seam and refuse cross-mode
analysis when corresponding poses differ. A water-region analysis must additionally bind an
explicit region that is present in both validated views and exclude the dialogue/HUD strip. Until
those controls pass, no missing interpolation target has been established by this three-run set.

### Replacement input and viewpoint contract

The mode-dependent trajectory was caused before capture: `SBR_PAD_SCRIPT` used the number of
`PADRead` calls as its key, while Native60 advances the live guest retrace counter by one per
simulation tick and Lerp60 advances it by two. Consequently `400:CSTICK=100/0` began near guest
retrace 400 in Native60 but near retrace 800 in Lerp60.

Capture schema 5 selects `SBR_PAD_SCRIPT_CLOCK=guest-retrace`. `native_pad` reads the same live
`__VIRetraceCount` used by the VI and presentation seams at each `PADRead`, and the pure PAD policy
maps that counter to the script key without mode-specific arithmetic. Read count remains the
runtime default outside this opt-in comparison contract. The comparator requires the runtime's
exact clock confirmation and binds `pad_script_clock: guest-retrace` in each manifest; a
read-count manifest fails closed. Its camera gate additionally binds the settled `j3dSys` 3x4 view
matrix at each required guest tick and rejects a missing predecessor or any same-tick pose
difference. Analysis defaults to grid ROI `[0,0..16,10)`, excluding the two bottom dialogue rows,
and refuses absent or static ROI content on either side.

These changes invalidate the previous capture set.

### Valid schema-5 water-facing result

The replacement three-run set completed on 2026-08-28. Native60, Native60-repeat, and Lerp60 each
produced 33 complete 1280x960 frames over guest retraces 1822 through 1854 with guarded exit 0.
Native60 repeated byte-for-byte; the camera matrices matched at every required guest tick; the
unique texture descriptor sets matched; and Lerp60 carried the required alternating `main/sub`
presentation roles. The forced-snap control duplicated every other frame, produced 16 of 32 exact
zero steps, and raised whole-ROI spatial alternation from 0.291 to 1.000. Instrument I038 is trusted
for the fields it covers.

The slow camera rotation keeps the sea visible. The water-focused grid ROI `[9,3..14,5)` improved
slightly from Native60 0.234 to Lerp60 0.227 mean spatial alternation. The broader sea-heavy ROI
`[8,2..14,5)` likewise improved from 0.261 to 0.254. This falsifies **water as a whole** as the
missing interpolation target in this view; it does not prove every water draw or animation is
correct.

The strongest Lerp60-only excess is cell `[10,2]` (normalized frame box
`[0.625,0.167..0.688,0.250]`), which worsened from 0.596 to 0.779. Inspection of the bound frames
shows that cell contains the moving palm trunk against sky, above the waterline. The screen-space
instrument cannot name its draw. The next grounded step is a same-run join from that cell to stable
draw identity; the cumulative interpolation population report is not sufficient attribution.

Evidence is under `scratch/frames/cadence_native60.rgba.*`,
`scratch/frames/cadence_native60_control.rgba.*`, `scratch/frames/cadence_lerp60.rgba.*`, and the
corresponding `scratch/logs/cadence_*.log` files.
