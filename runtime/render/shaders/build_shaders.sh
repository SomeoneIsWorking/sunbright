#!/usr/bin/env bash
# Regenerate the embedded SPIR-V headers from the GLSL sources.
set -e; cd "$(dirname "$0")"
gen() { glslc -fshader-stage=$2 --target-env=vulkan1.0 -O -mfmt=c "$1" -o /dev/stdout > .t
  { echo "// Auto-generated from $1 by glslc — DO NOT EDIT. Regenerate: build_shaders.sh"
    echo "#pragma once"; echo "#include <cstdint>"
    echo "static const uint32_t $3[] = $(cat .t);"; } > "$4"; rm -f .t; echo "wrote $4"; }
gen quad.vert.glsl vert kQuadVertSpv quad_vert_spv.h
gen quad.frag.glsl frag kQuadFragSpv quad_frag_spv.h
gen quad_ortho.vert.glsl vert kQuadOrthoVertSpv quad_ortho_vert_spv.h
gen quad_modulate.frag.glsl frag kQuadModulateFragSpv quad_modulate_frag_spv.h
