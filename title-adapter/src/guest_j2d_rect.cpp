#include <sunbright/title_adapter/guest_j2d_rect.h>

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

} // namespace sb::title_adapter
