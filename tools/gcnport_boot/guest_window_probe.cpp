// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_window_probe.h"

#include <sunbright/native_render/semantic_sink.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

constexpr std::size_t WINDOW_REGISTER = 3;
constexpr std::size_t OUTER_RECTANGLE_REGISTER = 4;
constexpr std::size_t CONTENTS_RECTANGLE_REGISTER = 5;
constexpr std::size_t PARENT_TRANSFORM_REGISTER = 6;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

// The role a resolved part names, back to the texture the window holds for it.
const sb::title_adapter::GuestJutTexture& texture_for(const sb::title_adapter::GuestWindow& window,
                                                      sb::native_render::WindowTextureRole role) {
    switch (role) {
    case sb::native_render::WindowTextureRole::TopLeft:
        return window.frameTextures[0];
    case sb::native_render::WindowTextureRole::TopRight:
        return window.frameTextures[1];
    case sb::native_render::WindowTextureRole::BottomLeft:
        return window.frameTextures[2];
    case sb::native_render::WindowTextureRole::BottomRight:
        return window.frameTextures[3];
    case sb::native_render::WindowTextureRole::Contents:
        break;
    }
    return window.contentsTexture;
}

// `drawContents` scales a corner's own alpha by the window's inherited alpha before writing the
// vertex, which is what makes a fading panel fade rather than switch off.
sb::native_render::Color contents_color(std::uint32_t rgba, std::uint8_t opacity) {
    sb::native_render::Color result = sb::native_render::color_from_rgba8(rgba);
    result.a = static_cast<float>(((rgba & 0xFFU) * opacity) / 0xFFU) / 255.0f;
    return result;
}

sb::native_render::ClipRect clip_of(const sb::native_render::PictureContext& context,
                                    const sb::title_adapter::GuestWindow& window) {
    if (!context.clipEnabled) {
        return {};
    }
    return {.enabled = true,
            .x = window.clipRect.x1,
            .y = window.clipRect.y1,
            .width = static_cast<std::uint32_t>(std::max(window.clipRect.width(), 0)),
            .height = static_cast<std::uint32_t>(std::max(window.clipRect.height(), 0))};
}

} // namespace

bool GuestWindowProbe::publish_part(gcnport::GuestContext& guest,
                                    const sb::native_render::WindowTexturedPart& part,
                                    const sb::title_adapter::GuestJutTexture& texture,
                                    const sb::native_render::PictureContext& context,
                                    const sb::title_adapter::GuestWindow& window,
                                    sb::native_render::Color black,
                                    sb::native_render::Color white) {
    const sb::native_render::DecodedTexture* decoded = nullptr;
    if (!textures_.resolve(guest, texture, decoded)) {
        return false;
    }
    sb::native_render::PictureCommand command{};
    if (!sb::native_render::make_window_picture_command(
            part, window.address + ++frameParts_, decoded->texture,
            static_cast<float>(window.colorAlpha) / 255.0f, black, white, command)) {
        return false;
    }
    command.clip = clip_of(context, window);
    submitted_ += 1;
    const sb::native_render::DecodedImageView image{
        decoded->texture.resource, decoded->texture.revision, decoded->texture.width,
        decoded->texture.height, decoded->rgba8};
    if (sb::native_render::submit_picture({context.canvas, command}, std::span(&image, 1))) {
        acceptedBySink_ += 1;
    }
    return true;
}

gcnport::HookResult GuestWindowProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;
    const auto address =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(WINDOW_REGISTER));
    const auto outerAddress = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(OUTER_RECTANGLE_REGISTER));
    const auto contentsAddress = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(CONTENTS_RECTANGLE_REGISTER));
    const auto transform = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(PARENT_TRANSFORM_REGISTER));

    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    sb::title_adapter::GuestWindow window{};
    const sb::title_adapter::GuestWindowError error =
        sb::title_adapter::read_guest_window(memory, address, window);
    windowErrors_[error] += 1;
    if (error != sb::title_adapter::GuestWindowError::None) {
        return gcnport::HookResult::call_original_once();
    }
    if (!window.hasFrame) {
        framelessWindows_ += 1;
    }

    const sb::native_render::PictureContext* const context =
        context_ != nullptr ? context_->current() : nullptr;
    if (context == nullptr) {
        withoutCanvas_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    const sb::title_adapter::GuestReader reader(memory);
    sb::title_adapter::GuestRect outer{};
    sb::title_adapter::GuestRect contents{};
    if (!sb::title_adapter::read_guest_rect(reader, outerAddress, outer) ||
        !sb::title_adapter::read_guest_rect(reader, contentsAddress, contents)) {
        unreadableRectangles_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    std::array<float, 12> parent{};
    if (!sb::title_adapter::read_guest_matrix(memory, transform, parent)) {
        unreadableTransform_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    sb::native_render::WindowLayout layout{};
    layout.outerWidth = outer.width();
    layout.outerHeight = outer.height();
    layout.minimumWidth = window.minimumWidth;
    layout.minimumHeight = window.minimumHeight;
    layout.contentsX1 = contents.x1;
    layout.contentsY1 = contents.y1;
    layout.contentsX2 = contents.x2;
    layout.contentsY2 = contents.y2;
    layout.mirror = window.mirror;
    layout.hasFrame = window.hasFrame;
    layout.hasContentsTexture = window.hasContentsTexture;
    for (std::size_t corner = 0; corner < layout.frameTextures.size(); ++corner) {
        layout.frameTextures[corner] = {window.frameTextures[corner].width,
                                        window.frameTextures[corner].height};
    }
    layout.contentsTexture = {window.contentsTexture.width, window.contentsTexture.height};
    std::copy_n(parent.begin(), parent.size(), layout.parentTransform.value.begin());
    std::copy_n(window.globalMatrix.begin(), window.globalMatrix.size(),
                layout.globalTransform.value.begin());

    // Retail's own no-op, taken before the budget so a window the title never drew does not spend
    // a draw another producer needed.
    if (sb::native_render::window_size_is_culled(layout)) {
        culledBySize_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    if (budget_ != nullptr && !budget_->take()) {
        withheldByBudget_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    sb::native_render::WindowGeometry geometry{};
    const sb::native_render::WindowLayoutResult result =
        sb::native_render::resolve_window_layout(layout, geometry);
    if (result != sb::native_render::WindowLayoutResult::Visible) {
        if (result == sb::native_render::WindowLayoutResult::Culled) {
            culledBySize_ += 1;
        } else {
            rejectedLayout_ += 1;
        }
        return gcnport::HookResult::call_original_once();
    }

    if (reports_ < maxReports_) {
        reports_ += 1;
        std::printf("gmse01_boot: window 0x%08x %dx%d at (%d, %d) alpha=%u, frame=%s mirror=0x%x, "
                    "contents (%d, %d)-(%d, %d)\n",
                    address, outer.width(), outer.height(), outer.x1, outer.y1, window.colorAlpha,
                    window.hasFrame ? "yes" : "no", window.mirror, contents.x1, contents.y1,
                    contents.x2, contents.y2);
    }

    if (!sb::native_render::has_semantic_sink()) {
        withoutSink_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    // The order below is `draw_private`'s own: the gradient, then whatever is stretched over it,
    // then the frame on top. A 2D pass that preserves submission order is what makes that an
    // ordering rather than a coincidence.
    if (geometry.contentsVisible) {
        const std::array<sb::native_render::Color, 4> colors{
            contents_color(window.contentsColors[0], window.colorAlpha),
            contents_color(window.contentsColors[1], window.colorAlpha),
            contents_color(window.contentsColors[2], window.colorAlpha),
            contents_color(window.contentsColors[3], window.colorAlpha)};
        sb::native_render::SolidRectangleCommand command{};
        if (sb::native_render::make_window_contents_command(geometry, window.address, colors,
                                                            command)) {
            command.clip = clip_of(*context, window);
            contentsFills_ += 1;
            submitted_ += 1;
            if (sb::native_render::submit_solid_rectangle({context->canvas, command})) {
                acceptedBySink_ += 1;
            }
        } else {
            rejectedLayout_ += 1;
        }
    }

    const sb::native_render::Color frameBlack =
        sb::native_render::color_from_rgba8(window.frameBlack);
    const sb::native_render::Color frameWhite =
        sb::native_render::color_from_rgba8(window.frameWhite);
    if (geometry.contentsTexture.visible) {
        contentsTextures_ += 1;
        // A contents texture is laid down flat: `drawContentsTexture` states the neutral pair
        // itself rather than inheriting the frame's.
        if (!publish_part(guest, geometry.contentsTexture,
                          texture_for(window, geometry.contentsTexture.texture), *context, window,
                          {}, {1.0f, 1.0f, 1.0f, 1.0f})) {
            return gcnport::HookResult::call_original_once();
        }
    }
    for (const sb::native_render::WindowTexturedPart& part : geometry.frame) {
        if (!part.visible) {
            continue;
        }
        if (!publish_part(guest, part, texture_for(window, part.texture), *context, window,
                          frameBlack, frameWhite)) {
            return gcnport::HookResult::call_original_once();
        }
    }
    return gcnport::HookResult::call_original_once();
}

void GuestWindowProbe::report() const {
    std::printf("gmse01_boot: guest window probe: %llu window(s) drawn, %llu quad(s) submitted, "
                "%llu accepted by the sink\n",
                static_cast<unsigned long long>(entries_),
                static_cast<unsigned long long>(submitted_),
                static_cast<unsigned long long>(acceptedBySink_));
    std::printf("gmse01_boot:   window errors:");
    for (const auto& [error, count] : windowErrors_) {
        std::printf(" %s=%llu", name(error), static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    std::printf("gmse01_boot:   %llu gradient fill(s), %llu contents texture(s), %llu frame "
                "piece(s), %llu window(s) with no frame\n",
                static_cast<unsigned long long>(contentsFills_),
                static_cast<unsigned long long>(contentsTextures_),
                static_cast<unsigned long long>(frameParts_),
                static_cast<unsigned long long>(framelessWindows_));
    std::printf("gmse01_boot:   not published: no_canvas=%llu unreadable_rectangles=%llu "
                "unreadable_transform=%llu withheld_by_budget=%llu culled_by_size=%llu "
                "rejected_layout=%llu no_sink=%llu\n",
                static_cast<unsigned long long>(withoutCanvas_),
                static_cast<unsigned long long>(unreadableRectangles_),
                static_cast<unsigned long long>(unreadableTransform_),
                static_cast<unsigned long long>(withheldByBudget_),
                static_cast<unsigned long long>(culledBySize_),
                static_cast<unsigned long long>(rejectedLayout_),
                static_cast<unsigned long long>(withoutSink_));
    textures_.report();
}

} // namespace sunbright::gcnport_boot
