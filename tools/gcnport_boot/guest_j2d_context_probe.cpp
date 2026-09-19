// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_j2d_context_probe.h"

#include <cstdio>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

constexpr std::size_t THIS_REGISTER = 3;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

[[nodiscard]] sb::native_render::PictureContext
picture_context_from(const sb::title_adapter::GuestOrthoGraph& graph) noexcept {
    return {{{static_cast<float>(graph.ortho.x1), static_cast<float>(graph.ortho.y1)},
             {static_cast<float>(graph.ortho.width()), static_cast<float>(graph.ortho.height())},
             {graph.bounds.x1, graph.bounds.y1, static_cast<std::uint32_t>(graph.bounds.width()),
              static_cast<std::uint32_t>(graph.bounds.height())}},
            sb::native_render::j2d_target_scissor(graph.scissorBounds.x1, graph.scissorBounds.y1,
                                                  graph.scissorBounds.x2, graph.scissorBounds.y2),
            // `J2DScreen::draw` is what decides whether a subtree clips to its parent, and this
            // probe does not sit on it. Left off here and counted by the picture probe, which is
            // what would be applying it.
            false};
}

} // namespace

gcnport::HookResult GuestJ2dContextProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;
    const auto context =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(THIS_REGISTER));

    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    sb::title_adapter::GuestOrthoGraph graph{};
    const sb::title_adapter::GuestGrafContextError error =
        sb::title_adapter::read_guest_ortho_graph(memory, context, {}, graph);
    errors_[error] += 1;
    if (error != sb::title_adapter::GuestGrafContextError::None) {
        return gcnport::HookResult::call_original_once();
    }

    const sb::native_render::PictureContext value = picture_context_from(graph);
    if (!sb::native_render::valid(value.canvas)) {
        invalidCanvas_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    accepted_ += 1;

    const bool changed = !has_ || value.canvas != current_.canvas;
    current_ = value;
    has_ = true;
    if (changed) {
        changes_ += 1;
        if (reports_ < maxReports_) {
            reports_ += 1;
            std::printf("gmse01_boot: J2D screen %gx%g at (%g, %g) into viewport %dx%d at (%d, %d)"
                        " (graf context 0x%08x)\n",
                        static_cast<double>(value.canvas.extent.x),
                        static_cast<double>(value.canvas.extent.y),
                        static_cast<double>(value.canvas.origin.x),
                        static_cast<double>(value.canvas.origin.y), value.canvas.viewport.width,
                        value.canvas.viewport.height, value.canvas.viewport.x,
                        value.canvas.viewport.y, graph.address);
        }
    }
    return gcnport::HookResult::call_original_once();
}

void GuestJ2dContextProbe::report() const {
    std::printf("gmse01_boot: guest J2D context probe: %llu setup(s), %llu accepted, %llu distinct "
                "screen(s)\n",
                static_cast<unsigned long long>(entries_),
                static_cast<unsigned long long>(accepted_),
                static_cast<unsigned long long>(changes_));
    std::printf("gmse01_boot:   graf context errors:");
    for (const auto& [error, count] : errors_) {
        std::printf(" %s=%llu", name(error), static_cast<unsigned long long>(count));
    }
    if (invalidCanvas_ != 0) {
        std::printf(" invalid_canvas=%llu", static_cast<unsigned long long>(invalidCanvas_));
    }
    std::printf("\n");
    if (!has_) {
        std::printf("gmse01_boot:   no orthographic J2D screen was ever established; every "
                    "picture this run saw had nowhere to be drawn\n");
    }
}

} // namespace sunbright::gcnport_boot
