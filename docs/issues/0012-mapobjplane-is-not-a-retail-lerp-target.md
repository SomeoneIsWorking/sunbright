---
id: 12
title: TMapObjPlane appears to need vertex interpolation but is dormant in retail scenes
status: dead-end
symptom: decomp source rebuilds deforming RockPlane and SandPlane geometry, suggesting an untagged 60 FPS interpolation target
tags: lerp,recomp,investigation,dead-end
created: 2026-08-26
updated: 2026-08-26
---

## Finding

`TMapObjPlane::draw` would require stable per-strip identity if a retail scene used it: the class
rebuilds its mutable height-map positions and normals as immediate-mode geometry. It is not a live
retail target, however. A controlled scan decompressed all 108 US retail scene archives and found
neither exact factory string, `RockPlane` nor `SandPlane`. The same scan found `MapObjGrass` and
`Mario` in `monte0.szs`, proving that serialized factory names were visible.

The decomp constructs these classes only through exact comparisons against those two absent names.
The proposed interpolation hook was therefore removed. See
`debug_journal/2026-08-26_mapobjplane_dormant_retail_code.md` for the scan boundary and control.

