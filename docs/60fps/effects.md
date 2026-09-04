# Effect behavior at 60 Hz

This document retains game-level findings needed to implement effect presentation without relying
on a retired execution design. No effect path is currently attached to gameplay.

## Water refraction

`TModelWaterManager::drawRefracAndSpec` (`GMSE01` `0x8027c12c`) samples a screen texture through a
projected texture matrix. The title constructs eye-space vertices from the current camera and emits
them directly. Those positions are already baked into the draw data, so changing only model
matrices cannot place the surface at an in-between camera pose.

The future native owner must derive both surface geometry and projected sampling from the same
interpolated camera state. A frame whose geometry is at one time and captured screen texture at
another will make the reflection slide across the surface. The detailed derivation is in
`docs/re_notes/water_refraction_projection.md`.

## Effect classes

| Class | State that must be owned |
|---|---|
| World geometry | previous/current pose or compatible source vertices |
| Projected screen texture | camera-consistent geometry, projection, and captured image |
| Cross-frame feedback | authored history advancement exactly once per game tick |
| Direct-transform effect | decoded direct transform values rather than a guessed matrix family |

The title's direct-transform effects bypass indexed model matrices. Blanket matrix substitution is
therefore incomplete and can corrupt unrelated 2D draws. Admit a family only at a named semantic
boundary with a forced control.

## Exact effect anchors

- `TShimmer::perform` at `0x8019f83c`: heat haze using the shared screen capture.
- `TAfterEffect::perform` at `0x8022d4f8`: dash blur with intentional prior-frame history.
- `TModelWaterManager::drawRefracAndSpec` at `0x8027c12c`: water refraction.
- `TBathWaterManager::draw_mist` at `0x801aa6cc`: self-contained EFB copy and redraw.
- `TMirrorCamera::perform` at `0x80193fbc`: second scene render into a mirror texture.

These addresses identify behavior; they do not authorize address-only hacks. Each hook needs exact
image identity, ABI fidelity, a renderer-neutral value contract, and ordinary JIT execution for the
unowned remainder.

## Falsified approaches

- Interpolating every direct XF transform also moves HUD/ortho state and does not identify water.
- Redirecting EFB addresses does not establish correct resource lifetime.
- Clearing the EFB changed frames without removing the reported motion artifact.
- A still image cannot prove that a reflection tracks during camera motion.

The qualifying test rotates the camera in a deterministic gameplay scene, proves the target effect
executed, compares consecutive presentation states, and includes an override-disabled control.
