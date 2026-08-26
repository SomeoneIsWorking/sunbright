#include "host/aurora_config.h"

#include <cassert>

int main() {
    const sb::host::AuroraConfigInput input{
        .userPath = "user/",
        .resourcesPath = "resources/",
        .windowWidth = 1280,
        .windowHeight = 960,
    };
    const AuroraConfig config = sb::host::make_aurora_config(input);

    // AuroraLogLevel starts at LOG_DEBUG, so a value-initialized config silently enables every
    // per-command diagnostic. That overloaded the render worker in optimized Debug builds.
    assert(config.logLevel == LOG_INFO);
    assert(config.desiredBackend == BACKEND_VULKAN);
    assert(config.vsync);
    assert(config.msaa == 1);
    assert(config.windowWidth == input.windowWidth);
    assert(config.windowHeight == input.windowHeight);
    assert(config.mem1Size == 0);
    assert(config.mem2Size == 0);
    return 0;
}
