---
id: C073
kind: claim
status: holds
created: 2026-08-27
tags:
depends: sms-recomp/runtime/render/native_tev_uniform.cpp#sbr_native_pack_tev_uniform, sms-recomp/runtime/shaders/geom.frag.glsl, sms-recomp/tests/tev_eval_test.cpp#test_ras_select_and_swap
---

## Claim

The recomp native SDL3-GPU shader receives and applies GX TEV RAS/TEX swap selectors and all four component-remap rows before TEV argument evaluation

## Evidence

native_tev_uniform_test first failed to compile because SbrNativeTevUniform had no swap table, then passed after exact selector/table transport was added; the independent SDK-derived tev_eval test covers identity, AAAA, and RGAA semantics. glslc compiled geom.frag.glsl, spirv-val accepted the Vulkan 1.0 binary, and the Clang sms-recomp target linked the regenerated embedded shader.

## What would falsify it

Any change to SbrNativeTevUniform layout/packing, geom.frag.glsl swap application, the embedded fragment SPIR-V, or the independent swap semantic test requires re-verification.
