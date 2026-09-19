// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_matrix_state.h"

#include <cstdio>

namespace sunbright::gcnport_boot {

void GuestMatrixState::load(std::uint32_t id, const sb::native_render::Matrix3x4& matrix) noexcept {
    loads_ += 1;
    if (id >= MATRIX_ROWS) {
        outOfRange_ += 1;
        return;
    }
    if (!loaded_[id]) {
        distinctRowsLoaded_ += 1;
        loaded_[id] = true;
    }
    matrices_[id] = matrix;
}

void GuestMatrixState::set_current(std::uint32_t id) noexcept {
    selections_ += 1;
    if (id >= MATRIX_ROWS) {
        outOfRange_ += 1;
        return;
    }
    current_ = id;
    everCurrent_[id] = true;
}

const sb::native_render::Matrix3x4* GuestMatrixState::current() const noexcept {
    return loaded_[current_] ? &matrices_[current_] : nullptr;
}

void GuestMatrixState::report() const {
    std::printf("gmse01_boot: guest matrix state: %llu load(s) into %llu row(s), %llu selection(s)"
                "\n",
                static_cast<unsigned long long>(loads_),
                static_cast<unsigned long long>(distinctRowsLoaded_),
                static_cast<unsigned long long>(selections_));
    std::printf("gmse01_boot:   rows ever current:");
    bool any = false;
    for (std::uint32_t row = 0; row < MATRIX_ROWS; ++row) {
        if (everCurrent_[row]) {
            std::printf(" %u", row);
            any = true;
        }
    }
    if (!any) {
        std::printf(" none set; row 0 is current because GX comes up that way");
    }
    std::printf("\n");
    if (outOfRange_ != 0 || unreadable_ != 0) {
        std::printf("gmse01_boot:   %llu identifier(s) outside matrix memory, %llu matrix/matrices "
                    "this could not read\n",
                    static_cast<unsigned long long>(outOfRange_),
                    static_cast<unsigned long long>(unreadable_));
    }
    if (const auto* matrix = current(); matrix != nullptr) {
        std::printf("gmse01_boot:   in force (row %u): [%g %g %g %g | %g %g %g %g | %g %g %g %g]\n",
                    current_, static_cast<double>(matrix->value[0]),
                    static_cast<double>(matrix->value[1]), static_cast<double>(matrix->value[2]),
                    static_cast<double>(matrix->value[3]), static_cast<double>(matrix->value[4]),
                    static_cast<double>(matrix->value[5]), static_cast<double>(matrix->value[6]),
                    static_cast<double>(matrix->value[7]), static_cast<double>(matrix->value[8]),
                    static_cast<double>(matrix->value[9]), static_cast<double>(matrix->value[10]),
                    static_cast<double>(matrix->value[11]));
    } else {
        std::printf("gmse01_boot:   no matrix is in force; an immediate-mode draw had no placement"
                    "\n");
    }
}

} // namespace sunbright::gcnport_boot
