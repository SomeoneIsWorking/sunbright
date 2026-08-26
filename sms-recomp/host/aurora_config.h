#pragma once

#include <aurora/aurora.h>

#include <cstdint>

namespace sb::host {

struct AuroraConfigInput {
    const char* userPath;
    const char* resourcesPath;
    std::uint32_t windowWidth;
    std::uint32_t windowHeight;
};

AuroraConfig make_aurora_config(const AuroraConfigInput& input) noexcept;

} // namespace sb::host
