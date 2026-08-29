#pragma once

#include <sunbright/native_render/sdl_gpu_calls.h>

#include <string>
#include <string_view>

namespace sb::native_render {

void assign_sdl_error(std::string& error, std::string_view operation, const SdlGpuCalls& calls);

} // namespace sb::native_render
