// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

#include <sunbright/native_render/semantic_2d_types.h>

namespace sunbright::gcnport_boot {

// The position matrix GMSE01's immediate-mode draws are placed by.
//
// A `J2DPicture` is handed its parent transform as an argument, so the picture probe can read the
// matrix a pane is drawn under without knowing anything about GX. An immediate-mode quad -- a
// glyph, a fader band -- is handed nothing: it pushes vertices and the hardware multiplies them by
// whichever position matrix is current. That matrix was loaded by an earlier, unrelated call, so
// the only way to know it at the draw is to have been watching.
//
// Both halves of that state are sticky and separate: `GXLoadPosMtxImm` writes one row of matrix
// memory and `GXSetCurrentMtx` chooses which row the next draw multiplies by. Following only the
// load would place a draw by a matrix the title had already switched away from.
class GuestMatrixState {
  public:
    // GX addresses position matrices by their first row of matrix memory, three rows apart, so the
    // ten `GX_PNMTX` identifiers run 0, 3 .. 27. Anything outside that is counted, not stored.
    static constexpr std::uint32_t MATRIX_ROWS = 32;

    void load(std::uint32_t id, const sb::native_render::Matrix3x4& matrix) noexcept;
    // Counted here rather than by the probe that feeds this, so one report carries every reason a
    // draw could go unplaced.
    void count_unreadable() noexcept { unreadable_ += 1; }
    void set_current(std::uint32_t id) noexcept;

    // The matrix in force, or null when nothing has been loaded into the current row yet.
    [[nodiscard]] const sb::native_render::Matrix3x4* current() const noexcept;

    void report() const;

  private:
    std::array<sb::native_render::Matrix3x4, MATRIX_ROWS> matrices_{};
    std::array<bool, MATRIX_ROWS> loaded_{};
    // GX comes up with `GX_PNMTX0` current, which is the row every 2D path in this title uses.
    std::uint32_t current_ = 0;

    std::uint64_t loads_ = 0;
    std::uint64_t selections_ = 0;
    std::uint64_t outOfRange_ = 0;
    std::uint64_t unreadable_ = 0;
    // How many rows the title ever loaded, and how many it ever made current: a run that only ever
    // used one says so, and a draw placed by the wrong one of two would not.
    std::uint64_t distinctRowsLoaded_ = 0;
    std::array<bool, MATRIX_ROWS> everCurrent_{};
};

} // namespace sunbright::gcnport_boot
