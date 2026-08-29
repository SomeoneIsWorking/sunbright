#include "sdl_gpu_error.h"

namespace sb::native_render {

void assign_sdl_error(std::string& error, std::string_view operation, const SdlGpuCalls& calls) {
    error.assign(operation);
    const char* detail = calls.getError != nullptr ? calls.getError() : nullptr;
    if (detail != nullptr && detail[0] != '\0') {
        error.append(": ");
        error.append(detail);
    }
}

} // namespace sb::native_render
