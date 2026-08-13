# Dolphin oracle: signed-16 far-grass interpolation

## Observation

An instrumented Dolphin FIFO recording (`scratch/oracle/resume_title.dff`) completed on
2026-08-13 with three 273,540-byte title frames. The existing graphics registry, originally
populated from Dolphin-observed runs, identified `TMapObjGrassGroup::drawFar` at `0x801e9880` as
the remaining direct signed-16 deforming path.

The initial guarded stage-8 audit confirmed the exact failure mode rather than merely assuming it:
`sub_801e9880+0x48` filed 292 `camera-only` draws. The generic vertex interpolator only accepted
direct f32 XYZ records, and `drawFar` had deliberately been left without a tag. The issue was not a
grass-specific motion rule.

## Fix

`aurora::gfx::interp::patch_vertices` now accepts direct big-endian signed-16 XYZ positions. It
records the VAT fractional shift with each `DrawData`, decodes samples into position units, lerps
there, and rounds/clamps back to the same signed-16 big-endian representation. `drawFar` now opens
the existing grass scope, so it uses the real `TMapObjGrassGroup*` identity and the same merge/count
guard as `drawNear`.

The interpolation self-test includes a signed-16, frac=1 case with both sign directions and checks
the encoded half-step. This prevents a host-endian or ignored-fraction regression from looking like
a successful pair.

## Verification

`ctest --test-dir build-sms-recomp -R '^(tev_eval|gx_light|interp_pairing)$'` passed.

`gpuguard run --timeout 150 -- ./run-safe.sh SBR_STAGE=8 SBR_SCENARIO=0 SBR_LERP60=1
SBR_QUIT_AFTER=400` completed with exit 0 and no kernel amdgpu event. Its final audit reports:

- `grass (deforming)`: 391 of 392 vertex draws lerped (99.7%); one first/no-previous sample.
- Grass seam: 11,368 `drawNear` and 11,368 `drawFar` calls reached; 11,368 immediate primitives
  were tagged (the state merger combines the two source classes into 392 vertex draws).
- No untagged indexed draw remains; the remaining untagged perspective draws are direct geometry.

The particle stripe residual remains unmodified: its 1,728 misses are all vertex-count changes, and
the prefix/suffix alignment distances (33.221 / 48.188) do not establish a common segment.
