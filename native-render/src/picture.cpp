#include <sunbright/native_render/picture.h>

#include <algorithm>
#include <cmath>

namespace sb::native_render {
namespace {

bool finite(Vec2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool finite(Color value) noexcept {
    return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b) &&
           std::isfinite(value.a);
}

float area2(Vec2 a, Vec2 b, Vec2 c) noexcept {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

float mix(float first, float second, float amount) noexcept {
    return first + (second - first) * amount;
}

Color remap(Color low, Color high, Color amount) noexcept {
    return {mix(low.r, high.r, amount.r), mix(low.g, high.g, amount.g),
            mix(low.b, high.b, amount.b), mix(low.a, high.a, amount.a)};
}

Color multiply(Color first, Color second) noexcept {
    return {first.r * second.r, first.g * second.g, first.b * second.b, first.a * second.a};
}

Color saturated(Color value) noexcept {
    return {std::clamp(value.r, 0.0f, 1.0f), std::clamp(value.g, 0.0f, 1.0f),
            std::clamp(value.b, 0.0f, 1.0f), std::clamp(value.a, 0.0f, 1.0f)};
}

Matrix3x4 concatenate(const Matrix3x4& first, const Matrix3x4& second) noexcept {
    Matrix3x4 result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            float value = column == 3 ? first.value[row * 4 + 3] : 0.0f;
            for (std::size_t inner = 0; inner < 3; ++inner) {
                value += first.value[row * 4 + inner] * second.value[inner * 4 + column];
            }
            result.value[row * 4 + column] = value;
        }
    }
    return result;
}

Vec2 transform_point(const Matrix3x4& matrix, float x, float y) noexcept {
    return {matrix.value[0] * x + matrix.value[1] * y + matrix.value[3],
            matrix.value[4] * x + matrix.value[5] * y + matrix.value[7]};
}

} // namespace

bool decode_address_mode(std::uint8_t raw, AddressMode& mode) noexcept {
    switch (raw) {
    case 0:
        mode = AddressMode::Clamp;
        return true;
    case 1:
        mode = AddressMode::Repeat;
        return true;
    case 2:
        mode = AddressMode::Mirror;
        return true;
    default:
        return false;
    }
}

bool decode_min_filter(std::uint8_t raw, FilterMode& filter, MipFilter& mip) noexcept {
    if (raw > 5)
        return false;
    filter = (raw == 1 || raw == 3 || raw == 5) ? FilterMode::Linear : FilterMode::Nearest;
    mip = raw < 2 ? MipFilter::None : (raw < 4 ? MipFilter::Nearest : MipFilter::Linear);
    return true;
}

bool decode_mag_filter(std::uint8_t raw, FilterMode& filter) noexcept {
    if (raw > 1)
        return false;
    filter = raw == 1 ? FilterMode::Linear : FilterMode::Nearest;
    return true;
}

bool decode_blend_factor(std::uint32_t packed, std::size_t textureIndex, float& factor) noexcept {
    if (textureIndex == 0 || textureIndex > 3)
        return false;
    const unsigned shift = static_cast<unsigned>((textureIndex - 1U) * 8U);
    factor = static_cast<float>((packed >> shift) & 0xffU) / 255.0f;
    return true;
}

bool valid(const PictureCommand& picture) noexcept {
    if (picture.instance == 0 ||
        (picture.clip.enabled && (picture.clip.width == 0 || picture.clip.height == 0)) ||
        !std::isfinite(picture.opacity) || picture.opacity < 0.0f || picture.opacity > 1.0f ||
        picture.material.textureCount == 0 || picture.material.textureCount > 4 ||
        !finite(picture.material.black) || !finite(picture.material.white)) {
        return false;
    }

    for (std::size_t index = 0; index < picture.positions.size(); ++index) {
        if (!finite(picture.positions[index]) || !finite(picture.uv[index]) ||
            !finite(picture.corner[index])) {
            return false;
        }
    }
    if (std::fabs(area2(picture.positions[0], picture.positions[1], picture.positions[3])) <=
            0.000001f &&
        std::fabs(area2(picture.positions[0], picture.positions[3], picture.positions[2])) <=
            0.000001f) {
        return false;
    }

    for (std::size_t index = 0; index < picture.material.textureCount; ++index) {
        const PictureTexture& texture = picture.material.textures[index];
        if (texture.resource == 0 || texture.width == 0 || texture.height == 0 ||
            !std::isfinite(texture.colorMix) || texture.colorMix < 0.0f ||
            texture.colorMix > 1.0f || !std::isfinite(texture.alphaMix) ||
            texture.alphaMix < 0.0f || texture.alphaMix > 1.0f) {
            return false;
        }
    }
    return true;
}

bool valid(const PictureDraw& draw) noexcept {
    return valid(draw.canvas) && valid(draw.picture);
}

bool resolve_picture_layout(const PictureLayout& layout, std::array<Vec2, 4>& positions,
                            std::array<Vec2, 4>& uv) noexcept {
    if (layout.width <= 0 || layout.height <= 0 || layout.textureWidth == 0 ||
        layout.textureHeight == 0 || layout.binding > 0x0f || layout.mirror > 0x03)
        return false;
    for (float value : layout.parentTransform.value) {
        if (!std::isfinite(value))
            return false;
    }
    for (float value : layout.globalTransform.value) {
        if (!std::isfinite(value))
            return false;
    }

    int renderX = 0;
    int renderY = 0;
    int width = layout.width;
    int height = layout.height;
    if (layout.horizontalWrap == 0) {
        const int textureExtent =
            static_cast<int>(layout.transpose ? layout.textureHeight : layout.textureWidth);
        if ((layout.binding & 8U) != 0) {
            if ((layout.binding & 4U) == 0 && textureExtent < width)
                width = textureExtent;
        } else if (textureExtent < width) {
            renderX =
                (layout.binding & 4U) != 0 ? width - textureExtent : (width - textureExtent) / 2;
            width = textureExtent;
        }
    }
    if (layout.verticalWrap == 0) {
        const int textureExtent =
            static_cast<int>(layout.transpose ? layout.textureWidth : layout.textureHeight);
        if ((layout.binding & 2U) != 0) {
            if ((layout.binding & 1U) == 0 && textureExtent < height)
                height = textureExtent;
        } else if (textureExtent < height) {
            renderY =
                (layout.binding & 1U) != 0 ? height - textureExtent : (height - textureExtent) / 2;
            height = textureExtent;
        }
    }
    if (width <= 0 || height <= 0)
        return false;

    bool bindUStart;
    bool bindUEnd;
    bool bindVStart;
    bool bindVEnd;
    if (!layout.transpose) {
        bindUStart =
            (layout.mirror & 2U) != 0 ? (layout.binding & 4U) != 0 : (layout.binding & 8U) != 0;
        bindUEnd =
            (layout.mirror & 2U) != 0 ? (layout.binding & 8U) != 0 : (layout.binding & 4U) != 0;
        bindVStart =
            (layout.mirror & 1U) != 0 ? (layout.binding & 1U) != 0 : (layout.binding & 2U) != 0;
        bindVEnd =
            (layout.mirror & 1U) != 0 ? (layout.binding & 2U) != 0 : (layout.binding & 1U) != 0;
    } else {
        bindUStart =
            (layout.mirror & 2U) != 0 ? (layout.binding & 1U) != 0 : (layout.binding & 2U) != 0;
        bindUEnd =
            (layout.mirror & 2U) != 0 ? (layout.binding & 2U) != 0 : (layout.binding & 1U) != 0;
        bindVStart =
            (layout.mirror & 1U) != 0 ? (layout.binding & 8U) != 0 : (layout.binding & 4U) != 0;
        bindVEnd =
            (layout.mirror & 1U) != 0 ? (layout.binding & 4U) != 0 : (layout.binding & 8U) != 0;
    }

    const float renderedU = static_cast<float>(layout.transpose ? height : width) /
                            static_cast<float>(layout.textureWidth);
    const float renderedV = static_cast<float>(layout.transpose ? width : height) /
                            static_cast<float>(layout.textureHeight);
    const auto interval = [](bool bindStart, bool bindEnd, float extent) {
        if (bindStart)
            return std::array<float, 2>{0.0f, bindEnd ? 1.0f : extent};
        if (bindEnd)
            return std::array<float, 2>{1.0f - extent, 1.0f};
        return std::array<float, 2>{0.5f - extent / 2.0f, 0.5f + extent / 2.0f};
    };
    auto u = interval(bindUStart, bindUEnd, renderedU);
    auto v = interval(bindVStart, bindVEnd, renderedV);
    if ((layout.mirror & 2U) != 0)
        std::swap(u[0], u[1]);
    if ((layout.mirror & 1U) != 0)
        std::swap(v[0], v[1]);

    if (!layout.transpose) {
        uv = {Vec2{u[0], v[0]}, Vec2{u[1], v[0]}, Vec2{u[0], v[1]}, Vec2{u[1], v[1]}};
    } else {
        uv = {Vec2{u[0], v[1]}, Vec2{u[0], v[0]}, Vec2{u[1], v[1]}, Vec2{u[1], v[0]}};
    }

    const Matrix3x4 transform = concatenate(layout.parentTransform, layout.globalTransform);
    positions = {
        transform_point(transform, static_cast<float>(renderX), static_cast<float>(renderY)),
        transform_point(transform, static_cast<float>(renderX + width),
                        static_cast<float>(renderY)),
        transform_point(transform, static_cast<float>(renderX),
                        static_cast<float>(renderY + height)),
        transform_point(transform, static_cast<float>(renderX + width),
                        static_cast<float>(renderY + height))};
    return true;
}

PictureMesh make_mesh(const PictureCommand& picture) noexcept {
    constexpr std::array<std::size_t, 6> kCornerOrder{0, 1, 3, 0, 3, 2};
    PictureMesh mesh{};
    for (std::size_t index = 0; index < mesh.size(); ++index) {
        const std::size_t corner = kCornerOrder[index];
        mesh[index] = {picture.positions[corner], picture.uv[corner], picture.corner[corner]};
    }
    return mesh;
}

Color shade(const PictureMaterial& material, const std::array<Color, 4>& textureSamples,
            Color corner, float opacity) noexcept {
    if (material.textureCount == 0 || material.textureCount > material.textures.size())
        return {};

    Color result = textureSamples[0];
    if (!material.textures[0].hasAlpha)
        result.a = 1.0f;
    for (std::size_t index = 1; index < material.textureCount; ++index) {
        Color sample = textureSamples[index];
        if (!material.textures[index].hasAlpha)
            sample.a = 1.0f;
        result.r = mix(result.r, sample.r, material.textures[index].colorMix);
        result.g = mix(result.g, sample.g, material.textures[index].colorMix);
        result.b = mix(result.b, sample.b, material.textures[index].colorMix);
        result.a = mix(result.a, sample.a, material.textures[index].alphaMix);
    }

    result = remap(material.black, material.white, result);
    result = multiply(result, corner);
    result.a *= std::clamp(opacity, 0.0f, 1.0f);
    return saturated(result);
}

} // namespace sb::native_render
