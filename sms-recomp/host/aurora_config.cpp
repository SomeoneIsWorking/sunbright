#include "aurora_config.h"

namespace sb::host {

AuroraConfig make_aurora_config(const AuroraConfigInput& input) noexcept {
    AuroraConfig config{};
    config.appName = "sunbright-recomp";
    config.userPath = input.userPath;
    config.resourcesPath = input.resourcesPath;
    config.desiredBackend = BACKEND_VULKAN;
    config.logLevel = LOG_INFO;
    config.msaa = 1;
    config.vsync = true;
    config.windowWidth = input.windowWidth;
    config.windowHeight = input.windowHeight;
    config.mem1Size = 0;
    config.mem2Size = 0;
    return config;
}

} // namespace sb::host
