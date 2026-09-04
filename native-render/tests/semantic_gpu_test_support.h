#pragma once

#include <sunbright/native_render/sdl_gpu_frame_target.h>
#include <sunbright/native_render/semantic_2d_pass.h>
#include <sunbright/native_render/semantic_3d_pass.h>
#include <sunbright/native_render/semantic_sink.h>

#include <cstdint>
#include <string>

namespace sb::native_render::test {

[[nodiscard]] Color pixel(const SemanticFramePixels& frame, std::uint32_t x, std::uint32_t y);
[[nodiscard]] bool near(float actual, float expected, float tolerance = 2.0F / 255.0F);
void require_color(Color actual, Color expected);
[[nodiscard]] std::uint64_t hash(const SemanticFramePixels& frame);

[[nodiscard]] bool encode_and_readback(Semantic2dPass& pass, const SemanticFrame& frame,
                                       const SdlGpuFrameTarget& target, SemanticFramePixels& output,
                                       std::string& error);
[[nodiscard]] bool encode_3d_and_readback(Semantic3dPass& pass, const SemanticFrame& frame,
                                          const SdlGpuFrameTarget& target,
                                          SemanticFramePixels& output, std::string& error);

} // namespace sb::native_render::test
