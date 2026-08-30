#include <sunbright/native_render/semantic_frame_mode.h>

#include <cstring>

namespace sb::native_render {

SemanticFrameMode parse_semantic_frame_mode(const char* value) noexcept {
    if (value == nullptr || std::strcmp(value, "off") == 0)
        return SemanticFrameMode::Disabled;
    if (std::strcmp(value, "audit") == 0)
        return SemanticFrameMode::Audit;
    if (std::strcmp(value, "preview") == 0)
        return SemanticFrameMode::Preview;
    return SemanticFrameMode::Invalid;
}

} // namespace sb::native_render
