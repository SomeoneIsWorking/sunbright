// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_texture_cache.h"

#include <cstdio>
#include <span>
#include <utility>

namespace sunbright::gcnport_boot {
namespace {

[[nodiscard]] sb::native_render::JutTextureFields
fields_of(const sb::title_adapter::GuestJutTexture& texture) noexcept {
    return {.format = texture.format,
            .alphaEnabled = texture.alphaEnabled != 0,
            .width = texture.width,
            .height = texture.height,
            .wrapS = texture.wrapS,
            .wrapT = texture.wrapT,
            .minFilter = texture.minFilter,
            .magFilter = texture.magFilter,
            .hasPalette = texture.palette.present(),
            .paletteFormat = texture.palette.format,
            .paletteEntries = texture.palette.entries};
}

} // namespace

bool GuestTextureCache::resolve(gcnport::GuestContext& guest,
                                const sb::title_adapter::GuestJutTexture& texture,
                                const sb::native_render::DecodedTexture*& decoded) {
    const Key key{texture.data, texture.format,
                  (static_cast<std::uint32_t>(texture.width) << 16U) | texture.height,
                  texture.palette.colorTable};
    if (const auto found = cache_.find(key); found != cache_.end()) {
        decoded = &found->second;
        return true;
    }

    const sb::native_render::JutTextureFields fields = fields_of(texture);
    sb::native_render::JutTexturePlan plan{};
    sb::native_render::JutTextureError error = sb::native_render::plan_jut_texture(fields, plan);
    if (error != sb::native_render::JutTextureError::None) {
        errors_[error] += 1;
        return false;
    }

    encodedBytes_.assign(plan.encodedBytes, 0);
    if (!guest.read_memory(texture.data, std::as_writable_bytes(std::span(encodedBytes_)))) {
        errors_[sb::native_render::JutTextureError::EncodedBytesTooShort] += 1;
        return false;
    }
    paletteBytes_.assign(plan.paletteBytes, 0);
    if (plan.paletteBytes != 0 &&
        !guest.read_memory(texture.palette.colorTable,
                           std::as_writable_bytes(std::span(paletteBytes_)))) {
        errors_[sb::native_render::JutTextureError::PaletteBytesTooShort] += 1;
        return false;
    }

    sb::native_render::DecodedTexture value{};
    error = sb::native_render::decode_jut_texture(fields, encodedBytes_, paletteBytes_, value);
    errors_[error] += 1;
    if (error != sb::native_render::JutTextureError::None) {
        return false;
    }
    // The GPU cache keys on this, so it names the resource the title drew rather than the bytes
    // this run happened to read: two panes sharing one texture must share one upload.
    value.texture.resource = texture.resource != 0 ? texture.resource : texture.data;
    decoded_ += 1;
    bytes_ += value.rgba8.size();
    decoded = &cache_.emplace(key, std::move(value)).first->second;
    return true;
}

void GuestTextureCache::report() const {
    std::printf("gmse01_boot:   textures: %llu decoded, %llu distinct, %llu byte(s)\n",
                static_cast<unsigned long long>(decoded_),
                static_cast<unsigned long long>(cache_.size()),
                static_cast<unsigned long long>(bytes_));
    if (!errors_.empty()) {
        std::printf("gmse01_boot:   texture decode results:");
        for (const auto& [error, count] : errors_) {
            std::printf(" %s=%llu", sb::native_render::jut_texture_error_name(error),
                        static_cast<unsigned long long>(count));
        }
        std::printf("\n");
    }
}

} // namespace sunbright::gcnport_boot
