#include <sunbright/native_render/window.h>

#include <sunbright/native_render/solid_rectangle.h>

#include <algorithm>
#include <cmath>

namespace sb::native_render {
namespace {

constexpr float kUvOne = 1.0f;

bool nonzero_area(const std::array<Vec2, 4>& positions) noexcept {
    const auto area2 = [](Vec2 a, Vec2 b, Vec2 c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    return std::fabs(area2(positions[0], positions[1], positions[3])) > 0.000001f ||
           std::fabs(area2(positions[0], positions[3], positions[2])) > 0.000001f;
}

std::array<Vec2, 4> quad_uv(float left, float top, float right, float bottom) noexcept {
    return {{{left, top}, {right, top}, {left, bottom}, {right, bottom}}};
}

bool resolve_quad(const Matrix3x4& transform, std::int32_t x, std::int32_t y, std::int32_t width,
                  std::int32_t height, std::array<Vec2, 4>& positions) noexcept {
    const TransformedS16RectangleLayout rectangle{x, y, x + width, y + height, transform};
    return resolve_transformed_s16_rectangle(rectangle, positions);
}

WindowTexturedPart make_part(WindowTextureRole texture, const Matrix3x4& transform, std::int32_t x,
                             std::int32_t y, std::int32_t width, std::int32_t height,
                             std::array<Vec2, 4> uv) noexcept {
    WindowTexturedPart part{texture, {}, uv, false};
    if (resolve_quad(transform, x, y, width, height, part.positions))
        part.visible = nonzero_area(part.positions);
    return part;
}

std::array<Vec2, 4> mirrored_uv(bool horizontal, bool vertical) noexcept {
    return quad_uv(horizontal ? kUvOne : 0.0f, vertical ? kUvOne : 0.0f, horizontal ? 0.0f : kUvOne,
                   vertical ? 0.0f : kUvOne);
}

bool valid_extent(WindowTextureExtent extent) noexcept {
    return extent.width != 0 && extent.height != 0;
}

} // namespace

bool window_size_is_culled(const WindowLayout& layout) noexcept {
    return layout.outerWidth < layout.minimumWidth || layout.outerHeight < layout.minimumHeight;
}

WindowLayoutResult resolve_window_layout(const WindowLayout& layout,
                                         WindowGeometry& geometry) noexcept {
    if (window_size_is_culled(layout))
        return WindowLayoutResult::Culled;
    if (!valid(layout.parentTransform) || !valid(layout.globalTransform))
        return WindowLayoutResult::Invalid;
    if (layout.hasFrame &&
        !std::ranges::all_of(layout.frameTextures,
                             [](WindowTextureExtent extent) { return valid_extent(extent); })) {
        return WindowLayoutResult::Invalid;
    }
    if (layout.hasContentsTexture && !valid_extent(layout.contentsTexture))
        return WindowLayoutResult::Invalid;

    const Matrix3x4 transform =
        concatenate_transform(layout.parentTransform, layout.globalTransform);
    WindowGeometry result{};

    if (layout.contentsX2 > layout.contentsX1 && layout.contentsY2 > layout.contentsY1) {
        if (!resolve_quad(transform, layout.contentsX1, layout.contentsY1,
                          layout.contentsX2 - layout.contentsX1,
                          layout.contentsY2 - layout.contentsY1, result.contentsPositions)) {
            return WindowLayoutResult::Invalid;
        }
        result.contentsVisible = nonzero_area(result.contentsPositions);
        if (layout.hasContentsTexture) {
            const float width = static_cast<float>(layout.contentsX2 - layout.contentsX1);
            const float height = static_cast<float>(layout.contentsY2 - layout.contentsY1);
            const float uExtent = width / static_cast<float>(layout.contentsTexture.width);
            const float vExtent = height / static_cast<float>(layout.contentsTexture.height);
            const float u1 = -(uExtent - 1.0f) * 0.5f;
            const float v1 = -(vExtent - 1.0f) * 0.5f;
            result.contentsTexture = make_part(
                WindowTextureRole::Contents, transform, layout.contentsX1, layout.contentsY1,
                layout.contentsX2 - layout.contentsX1, layout.contentsY2 - layout.contentsY1,
                quad_uv(u1, v1, u1 + uExtent, v1 + vExtent));
        }
    }

    if (layout.hasFrame) {
        const auto& topLeft = layout.frameTextures[0];
        const auto& topRight = layout.frameTextures[1];
        const auto& bottomLeft = layout.frameTextures[2];
        const auto& bottomRight = layout.frameTextures[3];
        const std::int32_t rightX =
            layout.outerWidth - static_cast<std::int32_t>(bottomRight.width);
        const std::int32_t bottomY =
            layout.outerHeight - static_cast<std::int32_t>(bottomRight.height);
        const std::int32_t topLeftWidth = static_cast<std::int32_t>(topLeft.width);
        const std::int32_t topLeftHeight = static_cast<std::int32_t>(topLeft.height);

        result.frame[0] =
            make_part(WindowTextureRole::TopLeft, transform, 0, 0, topLeftWidth, topLeftHeight,
                      mirrored_uv((layout.mirror & 0x80U) != 0, (layout.mirror & 0x40U) != 0));
        result.frame[1] = make_part(
            WindowTextureRole::TopRight, transform, rightX, 0,
            static_cast<std::int32_t>(topRight.width), static_cast<std::int32_t>(topRight.height),
            mirrored_uv((layout.mirror & 0x20U) != 0, (layout.mirror & 0x10U) != 0));
        result.frame[2] =
            make_part(WindowTextureRole::BottomLeft, transform, 0, bottomY,
                      static_cast<std::int32_t>(bottomLeft.width),
                      static_cast<std::int32_t>(bottomLeft.height),
                      mirrored_uv((layout.mirror & 0x08U) != 0, (layout.mirror & 0x04U) != 0));
        result.frame[3] =
            make_part(WindowTextureRole::BottomRight, transform, rightX, bottomY,
                      static_cast<std::int32_t>(bottomRight.width),
                      static_cast<std::int32_t>(bottomRight.height),
                      mirrored_uv((layout.mirror & 0x02U) != 0, (layout.mirror & 0x01U) != 0));

        const float topU = (layout.mirror & 0x20U) != 0 ? 1.0f : 0.0f;
        const float topV = (layout.mirror & 0x10U) != 0 ? 1.0f : 0.0f;
        result.frame[4] = make_part(
            WindowTextureRole::TopRight, transform, topLeftWidth, 0, rightX - topLeftWidth,
            static_cast<std::int32_t>(topRight.height), quad_uv(topU, topV, topU, 1.0f - topV));

        const float bottomU = (layout.mirror & 0x02U) != 0 ? 1.0f : 0.0f;
        const float bottomV = (layout.mirror & 0x01U) != 0 ? 1.0f : 0.0f;
        result.frame[5] =
            make_part(WindowTextureRole::BottomRight, transform, topLeftWidth, bottomY,
                      rightX - topLeftWidth, static_cast<std::int32_t>(bottomRight.height),
                      quad_uv(bottomU, bottomV, bottomU, 1.0f - bottomV));

        const float leftU = (layout.mirror & 0x08U) != 0 ? 1.0f : 0.0f;
        const float leftV = (layout.mirror & 0x04U) != 0 ? 1.0f : 0.0f;
        result.frame[6] =
            make_part(WindowTextureRole::BottomLeft, transform, 0, topLeftHeight,
                      static_cast<std::int32_t>(bottomLeft.width), bottomY - topLeftHeight,
                      quad_uv(leftU, leftV, 1.0f - leftU, leftV));

        const float rightU = (layout.mirror & 0x02U) != 0 ? 1.0f : 0.0f;
        const float rightV = (layout.mirror & 0x01U) != 0 ? 1.0f : 0.0f;
        result.frame[7] =
            make_part(WindowTextureRole::BottomRight, transform, rightX, topLeftHeight,
                      static_cast<std::int32_t>(bottomRight.width), bottomY - topLeftHeight,
                      quad_uv(rightU, rightV, 1.0f - rightU, rightV));
    }

    geometry = result;
    return WindowLayoutResult::Visible;
}

bool make_window_contents_command(const WindowGeometry& geometry, std::uint64_t instance,
                                  const std::array<Color, 4>& colors,
                                  SolidRectangleCommand& command) noexcept {
    if (!geometry.contentsVisible || instance == 0)
        return false;
    SolidRectangleCommand result{};
    result.instance = instance;
    result.source = SolidRectangleSource::J2dWindowContents;
    result.positions = geometry.contentsPositions;
    result.corner = colors;
    if (!valid(result))
        return false;
    command = result;
    return true;
}

bool make_window_picture_command(const WindowTexturedPart& part, std::uint64_t instance,
                                 const PictureTexture& texture, float opacity, Color black,
                                 Color white, PictureCommand& command) noexcept {
    if (!part.visible || instance == 0)
        return false;
    PictureCommand result{};
    result.instance = instance;
    result.source = PictureSource::J2dWindow;
    result.positions = part.positions;
    result.uv = part.uv;
    result.opacity = opacity;
    result.material.textureCount = 1;
    result.material.textures[0] = texture;
    result.material.black = black;
    result.material.white = white;
    result.corner.fill({1.0f, 1.0f, 1.0f, 1.0f});
    if (!valid(result))
        return false;
    command = result;
    return true;
}

} // namespace sb::native_render
