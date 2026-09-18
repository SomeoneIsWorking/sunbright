// SPDX-License-Identifier: GPL-2.0-or-later
#include "frame_image_file.h"

#include <cstdio>
#include <vector>

namespace sunbright::gcnport_boot {

bool write_ppm(const std::string& path, std::uint32_t width, std::uint32_t height,
               std::span<const std::uint8_t> rgba8, std::string& error) {
    const std::size_t expected = static_cast<std::size_t>(width) * height * 4U;
    if (width == 0 || height == 0 || rgba8.size() != expected) {
        error = "the frame's extent and its pixel count disagree";
        return false;
    }
    std::FILE* const file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        error = "could not open '" + path + "' for writing";
        return false;
    }
    std::vector<std::uint8_t> rgb;
    rgb.reserve(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t pixel = 0; pixel < rgba8.size(); pixel += 4U) {
        rgb.push_back(rgba8[pixel]);
        rgb.push_back(rgba8[pixel + 1U]);
        rgb.push_back(rgba8[pixel + 2U]);
    }
    const std::string header =
        "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    const bool wroteHeader = std::fwrite(header.data(), 1, header.size(), file) == header.size();
    const bool wrotePixels = std::fwrite(rgb.data(), 1, rgb.size(), file) == rgb.size();
    // A close can fail on a full or unwritable filesystem after every write appeared to succeed, so
    // the file is only good once it has closed cleanly.
    const bool closed = std::fclose(file) == 0;
    if (!wroteHeader || !wrotePixels || !closed) {
        error = "'" + path + "' was not written whole";
        return false;
    }
    return true;
}

} // namespace sunbright::gcnport_boot
