// glsl_compile — see glsl_compile.h. Thin glslang wrapper, Vulkan SPIR-V target.

#include "glsl_compile.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <cstdio>
#include <mutex>

namespace {
void ensure_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        glslang::InitializeProcess();
        std::atexit([] { glslang::FinalizeProcess(); });
    });
}
}  // namespace

std::vector<uint32_t> sb_compile_fragment_glsl(const std::string& src) {
    ensure_init();
    glslang::TShader shader(EShLangFragment);
    const char* str = src.c_str();
    shader.setStrings(&str, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    const TBuiltInResource* res = GetDefaultResources();
    if (!shader.parse(res, 450, false, EShMsgDefault)) {
        std::fprintf(stderr, "[glsl] parse failed: %s\n%s\n", shader.getInfoLog(), src.c_str());
        return {};
    }
    glslang::TProgram prog;
    prog.addShader(&shader);
    if (!prog.link(EShMsgDefault)) {
        std::fprintf(stderr, "[glsl] link failed: %s\n", prog.getInfoLog());
        return {};
    }
    std::vector<uint32_t> spv;
    glslang::GlslangToSpv(*prog.getIntermediate(EShLangFragment), spv);
    return spv;
}
