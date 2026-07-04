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

static std::vector<uint32_t> compile_stage(EShLanguage stage, const std::string& src) {
    ensure_init();
    glslang::TShader shader(stage);
    const char* str = src.c_str();
    shader.setStrings(&str, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
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
    glslang::GlslangToSpv(*prog.getIntermediate(stage), spv);
    return spv;
}

std::vector<uint32_t> sb_compile_fragment_glsl(const std::string& src) {
    return compile_stage(EShLangFragment, src);
}

std::vector<uint32_t> sb_compile_vertex_glsl(const std::string& src) {
    return compile_stage(EShLangVertex, src);
}
