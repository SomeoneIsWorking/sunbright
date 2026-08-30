#include <sunbright/native_render/semantic_frame_mode.h>

#include <cassert>

int main() {
    using sb::native_render::parse_semantic_frame_mode;
    using sb::native_render::SemanticFrameMode;

    assert(parse_semantic_frame_mode(nullptr) == SemanticFrameMode::Disabled);
    assert(parse_semantic_frame_mode("off") == SemanticFrameMode::Disabled);
    assert(parse_semantic_frame_mode("audit") == SemanticFrameMode::Audit);
    assert(parse_semantic_frame_mode("preview") == SemanticFrameMode::Preview);
    assert(parse_semantic_frame_mode("") == SemanticFrameMode::Invalid);
    assert(parse_semantic_frame_mode("1") == SemanticFrameMode::Invalid);
    assert(parse_semantic_frame_mode("visible") == SemanticFrameMode::Invalid);
}
