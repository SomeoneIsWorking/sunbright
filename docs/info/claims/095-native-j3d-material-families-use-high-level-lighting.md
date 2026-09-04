---
id: C095
kind: claim
status: holds
created: 2026-08-31
tags: renderer,j3d,lighting
depends: native-render/src/j3d_lit_material.cpp#classify_j3d_lit_textured_material, native-render/src/j3d_specular_material.cpp#classify_j3d_specular_textured_material, native-render/src/j3d_alpha_masked_material.cpp#classify_j3d_alpha_masked_material, native-render/src/j3d_lit_alpha_mask_material.cpp#classify_j3d_lit_alpha_mask_material, native-render/src/j3d_layered_material.cpp#classify_j3d_layered_material, native-render/src/j3d_tinted_layered_material.cpp#classify_j3d_tinted_layered_material, native-render/src/model.cpp#transform_vertex, decomp/sms/src/MarioUtil/LightUtil.cpp#TLightCommon::setLight
---

## Claim

The PC-native J3D material classifiers consume high-level ambient, one or two point lights,
directional-specular direction and shininess, decoded normals, texture bindings, authored colours,
and fog policy without consuming GX light state.

## Evidence

Focused CPU and watched GPU controls establish these exact supported equations:

- texture-free and single-texture diffuse/specular;
- diffuse multiplied by an authored solid colour, with texture alpha scaled fourfold before cutout;
- one texture supplying lit RGB and an independent second texture/UV set supplying alpha;
- vertex-colour diffuse plus three times directional specular;
- a two-texture layered material with a 3/8 detail contribution;
- vertex diffuse times authored tint plus twice the directional highlight, including the one-point-
  light orange-highlight variant; and
- fogged animated-tint two-texture materials with explicit source-alpha blend and depth-write policy.

Negative controls reject altered stages, alpha selectors, bindings, coordinates, missing normals or
lighting, unsupported light counts, and one-field raster changes. GPU controls independently change
only the selected colour, mask, detail, highlight, or fog contribution.

## What would falsify it

A classifier reads GX/XF/FIFO light state, an unsupported program is accepted, a supported equation
produces the wrong known-different channel, or source and shader controls disagree on light count,
normal, shininess, alpha, or texture ownership.
