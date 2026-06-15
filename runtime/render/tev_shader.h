#pragma once
// tev_shader (N5) — translate a captured J3DMaterial TEV combiner state
// (NgxTevState, read object-model from guest RAM by ngx_j3d_shape.cpp) into a
// native GLSL 450 fragment shader that reproduces the GX TEV pipeline faithfully.
//
// The math is ported from the GameCube/Wii TEV integer pipeline as documented in
// Dolphin's PixelShaderGen (re-derived here, NOT linked): per-stage color/alpha
// combiners run in integer [0,255] space — d (±) lerp(a,b,c), bias, scale, clamp,
// destination register (CPREV/C0/C1/C2) — including the compare modes, konst
// selection and raster/texture inputs.
//
// The generated shader expects (matching runtime/render/vk_mesh.cpp):
//   layout(location=0) in vec4 vColor;   // vertex color0 (raster-channel approx)
//   layout(location=1) in vec2 vUV;      // tex0 coords (single-texcoord slice)
//   layout(set=0,binding=0) uniform sampler2D tex;   // batch's bound texmap-0
//   layout(push_constant) Mat { ivec4 kcolor[4]; ivec4 tevreg[3]; };
//
// KNOWN FIRST-SLICE LIMITS (documented, not bandaids — later stages own them):
//   - one texture / one texcoord (multi-texmap stages sample the bound tex),
//   - TEV swap tables assumed identity, indirect stages ignored, no alpha test
//     (PE block not captured yet), no lighting (raster ≈ vertex color0).

#include "../ngx/ngx_render_data.h"
#include <string>

// Build the GLSL fragment-shader source for this material's TEV state.
std::string sb_tev_gen_fragment(const NgxTevState& st);
