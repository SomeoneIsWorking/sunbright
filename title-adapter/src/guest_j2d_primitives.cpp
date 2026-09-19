#include <sunbright/title_adapter/guest_j2d_primitives.h>

namespace sb::title_adapter {

bool read_guest_rect(const GuestReader& reader, GuestAddress address, GuestRect& out) noexcept {
    std::uint32_t values[4]{};
    for (std::size_t index = 0; index < 4; ++index) {
        if (!reader.word(address + static_cast<GuestAddress>(index * 4), values[index])) {
            return false;
        }
    }
    out = {static_cast<std::int32_t>(values[0]), static_cast<std::int32_t>(values[1]),
           static_cast<std::int32_t>(values[2]), static_cast<std::int32_t>(values[3])};
    return true;
}

bool read_guest_matrix(const GuestReader& reader, GuestAddress address,
                       std::array<float, 12>& out) noexcept {
    for (std::size_t index = 0; index < out.size(); ++index) {
        if (!reader.real(address + static_cast<GuestAddress>(index * 4), out[index])) {
            return false;
        }
    }
    return true;
}

bool read_guest_matrix(const GuestMemory& memory, GuestAddress matrix,
                       std::array<float, 12>& out) noexcept {
    if (memory.read == nullptr || matrix == 0) {
        return false;
    }
    return read_guest_matrix(GuestReader(memory), matrix, out);
}

} // namespace sb::title_adapter
