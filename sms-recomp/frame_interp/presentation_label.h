// Self-describing labels for presented-frame captures.

#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace sb::frame_interp {

enum class PresentationRole : uint8_t {
    Main,
    Sub,
};

using PresentationDumpLabel = std::array<char, 32>;

// `main` means the game-owned emission and `sub` means an additional presentation within that
// simulation tick. The guest retrace count is deliberately shared by both roles: a replay sample
// does not advance the game.
PresentationDumpLabel presentation_dump_label(PresentationRole role, uint32_t guestTick) noexcept;

// Validate Aurora's retained-sample coordinates and label the sample as `sub`. For count > 2 every
// retained sample has the same role/tick label; the dump sequence number preserves their order.
// Invalid coordinates are refused so a broken callback cannot silently stamp a replay as real.
std::optional<PresentationDumpLabel> replay_sample_dump_label(uint32_t guestTick, unsigned index,
                                                              unsigned count) noexcept;

} // namespace sb::frame_interp
