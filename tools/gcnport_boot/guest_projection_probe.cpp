// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_projection_probe.h"

#include <sunbright/native_render/j3d_projection.h>

#include <cstdio>
#include <cstring>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

constexpr std::size_t MATRIX_REGISTER = 3;
constexpr std::size_t TYPE_REGISTER = 4;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

// Distinguishes one projection from another without keeping every matrix: the six values the
// hardware itself keeps are what make two projections different.
[[nodiscard]] std::uint64_t fingerprint(const sb::native_render::Matrix4x4& matrix) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::size_t index : {0U, 2U, 3U, 5U, 6U, 7U, 10U, 11U}) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &matrix.value[index], sizeof(bits));
        hash = (hash ^ bits) * 1099511628211ULL;
    }
    return hash;
}

} // namespace

gcnport::HookResult GuestProjectionProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;
    const auto matrix =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(MATRIX_REGISTER));
    const auto type = static_cast<std::uint32_t>(guest.general_register(TYPE_REGISTER));

    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    sb::native_render::Matrix4x4 projection{};
    sb::title_adapter::GuestProjectionKind kind{};
    const sb::title_adapter::GuestProjectionError error =
        sb::title_adapter::read_guest_projection(memory, matrix, type, projection, kind);
    errors_[error] += 1;
    if (error != sb::title_adapter::GuestProjectionError::None) {
        return gcnport::HookResult::call_original_once();
    }
    kinds_[kind] += 1;
    published_ += 1;
    sb::native_render::publish_j3d_projection(projection);

    // The same read answers a second question: which screen the title's un-owned 2D is in. An
    // orthographic matrix states one, a perspective matrix retires it.
    if (screenSpace_ != nullptr) {
        if (kind == sb::title_adapter::GuestProjectionKind::Orthographic) {
            sb::title_adapter::GuestOrthographicScreen screen{};
            const sb::title_adapter::GuestOrthographicScreenError screenError =
                sb::title_adapter::read_orthographic_screen(projection, kind, screen);
            screenErrors_[screenError] += 1;
            if (screenError == sb::title_adapter::GuestOrthographicScreenError::None) {
                screenSpace_->set_orthographic(screen);
            }
        } else {
            screenSpace_->set_perspective();
        }
    }

    const std::uint64_t id = fingerprint(projection);
    if (distinct_.size() < MAX_DISTINCT_PROJECTIONS) {
        const bool added = distinct_.insert(id).second;
        if (added && reports_ < maxReports_) {
            reports_ += 1;
            std::printf(
                "gmse01_boot: projection %s scale=(%g, %g) offset=(%g, %g) "
                "depth=(%g, %g)\n",
                name(kind), static_cast<double>(projection.value[0]),
                static_cast<double>(projection.value[5]), static_cast<double>(projection.value[2]),
                static_cast<double>(projection.value[6]), static_cast<double>(projection.value[10]),
                static_cast<double>(projection.value[11]));
        }
    } else if (!distinct_.contains(id)) {
        distinctPast_ += 1;
    }
    return gcnport::HookResult::call_original_once();
}

void GuestProjectionProbe::report() const {
    std::printf(
        "gmse01_boot: guest projection probe: %llu set(s), %llu published, %llu distinct%s\n",
        static_cast<unsigned long long>(entries_), static_cast<unsigned long long>(published_),
        static_cast<unsigned long long>(distinct_.size()),
        distinctPast_ != 0 ? " (a floor: more were seen than tracked)" : "");
    std::printf("gmse01_boot:   projection errors:");
    for (const auto& [error, count] : errors_) {
        std::printf(" %s=%llu", name(error), static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    if (kinds_.empty()) {
        std::printf("gmse01_boot:   no projection was read whole\n");
        return;
    }
    std::printf("gmse01_boot:   projection kinds:");
    for (const auto& [kind, count] : kinds_) {
        std::printf(" %s=%llu", name(kind), static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    if (!screenErrors_.empty()) {
        std::printf("gmse01_boot:   orthographic screens recovered:");
        for (const auto& [error, count] : screenErrors_) {
            std::printf(" %s=%llu", name(error), static_cast<unsigned long long>(count));
        }
        std::printf("\n");
    }
}

} // namespace sunbright::gcnport_boot
