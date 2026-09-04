#pragma once

#include <cstdint>
#include <string>

namespace sb {

struct RuntimeConfig {
    std::uint32_t windowWidth = 1280;
    std::uint32_t windowHeight = 960;
    std::uint64_t quitAfter = 0;
    double maxPresentHz = 120.0;
    unsigned watchdogSeconds = 5;
    bool turbo = false;
    bool traceSequence = false;
    bool audioDebug = false;
    bool headless = false;
    bool fifoCopySync = true;
    bool wipeDebug = false;
    std::string romPath = "rom.rvz";
    std::string fifoReplayPath;
    std::string semanticFrameMode;
    std::string audioRawPath;
    std::string pinStatePath;
    std::string padScript;
    std::string logChannels;
};

bool configure_runtime(std::string& error);
const RuntimeConfig& runtime_config();

} // namespace sb
