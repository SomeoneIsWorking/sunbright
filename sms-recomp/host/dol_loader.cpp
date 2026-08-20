#include "dol_loader.h"

#include "intrinsics.h"

#include <lucent/log.h>

#include <cstdio>

namespace sb::host {
namespace {

std::uint32_t be32(const std::uint8_t *bytes) {
  return std::uint32_t(bytes[0]) << 24 | std::uint32_t(bytes[1]) << 16 |
         std::uint32_t(bytes[2]) << 8 | bytes[3];
}

void guest_write(std::uint32_t address, const std::uint8_t *source,
                 std::uint32_t size) {
  for (std::uint32_t i = 0; i < size; ++i)
    if (std::uint8_t *destination = sb_ram_fast(address + i))
      *destination = source[i];
}

} // namespace

bool load_dol(const std::string &path, DolImage &out) {
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    lucent::error("dol", "cannot open {}", path);
    return false;
  }
  std::fseek(file, 0, SEEK_END);
  const long fileSize = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (fileSize < 0x100) {
    std::fclose(file);
    lucent::error("dol", "short read on {}", path);
    return false;
  }
  out.bytes.resize(static_cast<std::size_t>(fileSize));
  const std::size_t read =
      std::fread(out.bytes.data(), 1, out.bytes.size(), file);
  std::fclose(file);
  if (read != out.bytes.size()) {
    lucent::error("dol", "short read on {}", path);
    return false;
  }

  const std::uint8_t *header = out.bytes.data();
  for (int i = 0; i < 18; ++i) {
    const std::uint32_t fileOffset = be32(header + i * 4);
    const std::uint32_t guestAddress = be32(header + 0x48 + i * 4);
    const std::uint32_t size = be32(header + 0x90 + i * 4);
    if (fileOffset != 0 && size != 0)
      out.sections.push_back({fileOffset, guestAddress, size});
  }
  out.bssAddress = be32(header + 0xD8);
  out.bssSize = be32(header + 0xDC);
  out.entry = be32(header + 0xE0);
  return true;
}

void install_dol(const DolImage &image) {
  // BSS is cleared first because this DOL declares a BSS range containing a
  // loaded data section. Loaded data must win.
  for (std::uint32_t i = 0; i < image.bssSize; ++i)
    if (std::uint8_t *byte = sb_ram_fast(image.bssAddress + i))
      *byte = 0;
  for (const DolSection &section : image.sections) {
    guest_write(section.guestAddress, image.bytes.data() + section.fileOffset,
                section.size);
    lucent::debug("dol", "section -> 0x{:08x} +0x{:x}", section.guestAddress,
                  section.size);
  }
}

std::uint32_t DolImage::arena_low() const noexcept {
  std::uint32_t result = bssAddress + bssSize;
  for (const DolSection &section : sections)
    if (section.guestAddress + section.size > result)
      result = section.guestAddress + section.size;
  return result;
}

} // namespace sb::host
