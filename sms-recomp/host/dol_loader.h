#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sb::host {

struct DolSection {
  std::uint32_t fileOffset;
  std::uint32_t guestAddress;
  std::uint32_t size;
};

struct DolImage {
  std::uint32_t entry = 0;
  std::uint32_t bssAddress = 0;
  std::uint32_t bssSize = 0;
  std::vector<DolSection> sections;
  std::vector<std::uint8_t> bytes;

  std::uint32_t arena_low() const noexcept;
};

bool load_dol(const std::string &path, DolImage &out);
void install_dol(const DolImage &image);

} // namespace sb::host
