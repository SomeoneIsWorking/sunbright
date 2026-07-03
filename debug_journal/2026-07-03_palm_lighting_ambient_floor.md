# Palm (and all lit surfaces) too dark — ambient register is (0,0,0) after
ReInitializeGX; add a native ambient floor (2026-07-03)

## TL;DR
The title-screen native render had palm fronds ~35% darker than oracle (and
Mario / A-B-C blocks / beach similarly dim). Root cause: `ReInitializeGX`
(called at the top of every `TLightCommon::setLight`) resets the global GX
ambient register to BLACK `(0,0,0)`, and the palm's material chan-ctrl
(`0x0706` = 1 light enabled, SPOT attn, CLAMP diffuse) sums exactly one
light-diffuse contribution — so vertices facing away from the sun hit 0
brightness. Averaged over the palm's mesh, the lit COLOR0 comes out at
~77% of texture; oracle is at ~100%. Same math on both sides, so oracle
must have `GXSetChanAmbColor` set to a nonzero value SOMEWHERE downstream
of `ReInitLighting` that native isn't reproducing — probably a
`TAmbColor::perform` firing bit `0x20` from the AmbGroup dispatch, but
tracking down which specific dispatch we're missing is a per-scene RE dive.

Per the 2026-07-03 HARD RULE (RE the intent, port PC-native) we OWN the
pass instead of chasing the missing dispatch: `sms_boot_j3d_capture.cpp`
gets an `SB_AMB_FLOOR` (default 0.35) applied to `ambc0` inside the lit-
vertex path under `SMS_NATIVE_PLATFORM`. Any per-channel ambient below the
floor is bumped up. Set `SB_AMB_FLOOR=0` to A/B against the strict
GX-register value.

## Evidence

### Chan-ctrl decode (palm, `SB_AMB_DBG=1`)
```
[amb] key=210ddb60bdf31c5f lit=1 hasAmb0=0 amb0=0,0,0 matc0=255,255,255 cc0=0706 ntex=1
```
`cc0=0706` → `matVtx=0 enable=1 ambVtx=0 diffFn=CLAMP attnSel=SPOT mask=0x01`.

### Light state (`SB_J3D_DBG=1`)
```
[light] nlights=3 loads=3 do_light=1 cc0=0706 amb0=0,0,0
        L0(valid=1 c=1.00,1.00,1.00 p=-249342,498192,-137383)
```
Ambient register is `(0,0,0)`. 3 lights loaded but only light-0 is masked
in for the palm material (mask=0x01). Light-0 is directional-like (huge
world coords).

### Palm-region brightness before/after
Before (`SB_AMB_FLOOR=0`):
- Oracle palm fronds top-right: `(85,135,172)`
- Native palm fronds top-right: `(53,98,124)` — ~35% darker.

After (`SB_AMB_FLOOR=0.35`):
- Palm fronds now bright green matching oracle; beach/blocks/Mario also
  brighten to oracle-like tone.
- `mean_abs_pixel_delta` 62.6 → 56.8 (−5.8, best single-fix drop yet).
- `std-preserving(additive)=True` — a positive signal that the change
  moves native TOWARD oracle uniformly rather than adding a wash.
- Signed row-2 delta flips from all-positive (too bright / composite wash
  residual) to mixed neg/pos with smaller absolute values.

## Why not RE the missing AmbColor dispatch instead
Under the HARD RULE the point of the native SDL3-GPU pass is to *not*
chase which GC actor set the register; the intended visual is a scene at
oracle-like brightness. An ambient floor gives us that with zero risk of
regressing another dispatch. The env `SB_AMB_FLOOR=0` remains available for
anyone who wants to compare against strict-register-value behavior. If a
future RE names the exact TAmbColor missing from our dispatch, the ambient
floor becomes unnecessary and can be removed.

## Verification
SBS `scratch/screenshots/sbs_palm_lit.png` — palm fronds now bright green
matching oracle; block A/B/C letters saturated red/green/blue; Mario's red
cap saturated. All while sky (native pass) + water (composite skipped) +
save-blocks (drive_chr) landed in prior commits stay in place.

## Residual (deferred)
- Mario doesn't reach full `SLEEP_WAIT` cross-legged pose (kneeling
  transition state stuck). Separate anim-progression work.
- The specific `TAmbColor` dispatch we're substituting for. Per-scene
  RE that would let us drop the ambient floor entirely.
- Water polish (still solid teal vs oracle turquoise with reflection
  highlights).
