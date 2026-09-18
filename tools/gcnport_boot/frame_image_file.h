// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <string>

// Writing one rendered frame out as an image file.
//
// A separate owner from the renderer because it is a file format, not a frame lifetime: it knows
// nothing about GPUs, bridges or the guest, and the renderer knows nothing about pixel layouts on
// disk. P6 is the format this repository's own comparison tools already read
// (`tools/render/ab_diff.py`, `tools/render/sb_oracle_diff.py`), so a frame written here can be
// diffed against a Dolphin capture without a converter in between.

namespace sunbright::gcnport_boot {

// `rgba8` is tightly packed, top row first, and must hold exactly `width * height * 4` bytes.
// Alpha is dropped: P6 carries three channels, and an image compared against a console capture is
// compared on what the console displayed.
[[nodiscard]] bool write_ppm(const std::string& path, std::uint32_t width, std::uint32_t height,
                             std::span<const std::uint8_t> rgba8, std::string& error);

} // namespace sunbright::gcnport_boot
