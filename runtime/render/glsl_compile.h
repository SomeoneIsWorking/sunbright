#pragma once
// glsl_compile — our own GLSL 450 → SPIR-V compiler (glslang directly, no Dolphin
// header injection). Used to compile the per-material TEV fragment shaders
// (tev_shader) at runtime. glslang is a standalone library (vendored in the
// Dolphin tree); this keeps shader compilation Dolphin-renderer-independent —
// unlike Vulkan::ShaderCompiler, which prepends Dolphin's own VideoCommon header
// (with its own #version) and would clash with a complete standalone shader.
#include <cstdint>
#include <string>
#include <vector>

// Compile a complete GLSL 450 fragment shader (must contain its own #version) to
// SPIR-V words. Returns empty on failure (the glslang log is printed to stderr).
std::vector<uint32_t> sb_compile_fragment_glsl(const std::string& src);
