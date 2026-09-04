---
id: C039
kind: claim
status: holds
created: 2026-08-12
tags: 
---

## Claim

Interpolation measurably smooths the presented image during a controlled camera rotation: its
whole-gameplay spatial alternation is lower than Native60 over the same guest time and exact camera
trajectory.

## Evidence

A schema-5 bounded capture on 2026-08-28, Delfino guest retraces 1822..1854,
with guest-retrace-keyed slow camera rotation. Native60 repeated byte-for-byte and every cross-mode
settled camera matrix matched. Across grid ROI `[0,0..16,10)`, excluding the dialogue band,
Native60 mean spatial alternation was 0.291 and Lerp60 was 0.245. The planted every-other-frame
snap control raised the same Native60 baseline to 1.000. This screen-cell metric does not identify
the draw that produced a pixel, prove scanout timing, or prevent a region-wide improvement from
concealing one worse subregion.

The old 2026-08-12 `PADRead`-count comparison is superseded: the same script advanced at different
guest times in Native60 and Lerp60 and therefore did not establish the same camera trajectory.

## What would falsify it

a change to interpolation, replay emission, camera/input timing, renderer output, or the comparator;
re-run the complete schema-5 Native60 / repeat / Lerp60 control
