// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_picture_probe.h"

#include <sunbright/native_render/semantic_sink.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

constexpr std::size_t PANE_REGISTER = 3;
constexpr std::size_t PARENT_TRANSFORM_REGISTER = 6;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

} // namespace

gcnport::HookResult GuestPictureProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;
    const auto pane =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(PANE_REGISTER));
    const auto transform = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(PARENT_TRANSFORM_REGISTER));

    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    sb::title_adapter::GuestPicture picture{};
    const sb::title_adapter::GuestPictureError error =
        sb::title_adapter::read_guest_picture(memory, pane, picture);
    pictureErrors_[error] += 1;
    if (error != sb::title_adapter::GuestPictureError::None) {
        return gcnport::HookResult::call_original_once();
    }
    textureCounts_[picture.textureCount] += 1;
    if (picture.clipRect.width() < picture.bounds.width() ||
        picture.clipRect.height() < picture.bounds.height()) {
        panesWithNarrowerClip_ += 1;
    }

    const sb::native_render::PictureContext* const context =
        context_ != nullptr ? context_->current() : nullptr;
    if (context == nullptr) {
        withoutCanvas_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    std::array<float, 12> parent{};
    if (!sb::title_adapter::read_guest_matrix(memory, transform, parent)) {
        unreadableTransform_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    if (budget_ != nullptr && !budget_->take()) {
        withheldByBudget_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    sb::native_render::PictureCommand command{};
    command.instance = pane;
    command.source = sb::native_render::PictureSource::J2dPicture;
    command.opacity = static_cast<float>(picture.colorAlpha) / 255.0f;
    command.material.textureCount = picture.textureCount;
    command.material.black = sb::native_render::color_from_rgba8(picture.black);
    command.material.white = sb::native_render::color_from_rgba8(picture.white);
    for (std::size_t corner = 0; corner < command.corner.size(); ++corner) {
        command.corner[corner] = sb::native_render::color_from_rgba8(picture.cornerColors[corner]);
    }

    std::array<sb::native_render::DecodedImageView, sb::title_adapter::GUEST_MAX_PICTURE_TEXTURES>
        images{};
    for (std::size_t layer = 0; layer < picture.textureCount; ++layer) {
        const sb::native_render::DecodedTexture* decoded = nullptr;
        if (!textures_.resolve(guest, picture.textures[layer], decoded)) {
            return gcnport::HookResult::call_original_once();
        }
        sb::native_render::PictureTexture& texture = command.material.textures[layer];
        texture = decoded->texture;
        // Layer zero is the base; every layer above it is mixed in by a constant the picture
        // stores packed, one nibble pair per layer.
        if (layer != 0 && (!sb::native_render::decode_blend_factor(picture.blendKonstColor, layer,
                                                                   texture.colorMix) ||
                           !sb::native_render::decode_blend_factor(picture.blendKonstAlpha, layer,
                                                                   texture.alphaMix))) {
            invalidBlendFactor_ += 1;
            return gcnport::HookResult::call_original_once();
        }
        images[layer] = {decoded->texture.resource, decoded->texture.revision,
                         decoded->texture.width, decoded->texture.height, decoded->rgba8};
    }

    sb::native_render::PictureLayout layout{};
    layout.width = picture.bounds.width();
    layout.height = picture.bounds.height();
    layout.textureWidth = command.material.textures[0].width;
    layout.textureHeight = command.material.textures[0].height;
    layout.binding = picture.binding;
    layout.mirror = picture.mirror;
    layout.transpose = picture.flip;
    layout.horizontalWrap = picture.wrapHorizontal;
    layout.verticalWrap = picture.wrapVertical;
    std::copy_n(parent.begin(), parent.size(), layout.parentTransform.value.begin());
    std::copy_n(picture.globalMatrix.begin(), picture.globalMatrix.size(),
                layout.globalTransform.value.begin());
    if (!sb::native_render::resolve_picture_layout(layout, command.positions, command.uv)) {
        unresolvedLayout_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    if (reports_ < maxReports_) {
        reports_ += 1;
        std::printf("gmse01_boot: picture 0x%08x %dx%d at (%d, %d) alpha=%u, %u texture(s), first "
                    "%ux%u fmt%u\n",
                    pane, picture.bounds.width(), picture.bounds.height(), picture.bounds.x1,
                    picture.bounds.y1, picture.colorAlpha, picture.textureCount,
                    command.material.textures[0].width, command.material.textures[0].height,
                    picture.textures[0].format);
    }

    if (!sb::native_render::has_semantic_sink()) {
        withoutSink_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    submitted_ += 1;
    const sb::native_render::PictureDraw draw{context->canvas, command};
    if (sb::native_render::submit_picture(draw, std::span(images).first(picture.textureCount))) {
        acceptedBySink_ += 1;
    }
    return gcnport::HookResult::call_original_once();
}

void GuestPictureProbe::report() const {
    std::printf("gmse01_boot: guest picture probe: %llu pane(s) drawn, %llu submitted, %llu "
                "accepted by the sink\n",
                static_cast<unsigned long long>(entries_),
                static_cast<unsigned long long>(submitted_),
                static_cast<unsigned long long>(acceptedBySink_));
    std::printf("gmse01_boot:   picture errors:");
    for (const auto& [error, count] : pictureErrors_) {
        std::printf(" %s=%llu", name(error), static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    std::printf("gmse01_boot:   not published: no_canvas=%llu unreadable_transform=%llu "
                "withheld_by_budget=%llu unresolved_layout=%llu invalid_blend_factor=%llu "
                "no_sink=%llu\n",
                static_cast<unsigned long long>(withoutCanvas_),
                static_cast<unsigned long long>(unreadableTransform_),
                static_cast<unsigned long long>(withheldByBudget_),
                static_cast<unsigned long long>(unresolvedLayout_),
                static_cast<unsigned long long>(invalidBlendFactor_),
                static_cast<unsigned long long>(withoutSink_));
    textures_.report();
    if (!textureCounts_.empty()) {
        std::printf("gmse01_boot:   layers per pane:");
        for (const auto& [count, panes] : textureCounts_) {
            std::printf(" %u=%llu", count, static_cast<unsigned long long>(panes));
        }
        std::printf("\n");
    }
    std::printf("gmse01_boot:   %llu pane(s) clip to less than their bounds; this probe does not "
                "apply a clip, so that count is the size of the gap\n",
                static_cast<unsigned long long>(panesWithNarrowerClip_));
}

} // namespace sunbright::gcnport_boot
