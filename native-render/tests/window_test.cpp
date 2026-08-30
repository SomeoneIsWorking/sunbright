#include <sunbright/native_render/window.h>

#include <cmath>
#include <cstdio>

namespace {

using namespace sb::native_render;

Matrix3x4 identity() {
    return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
}

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.00001f;
}

bool check(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "window_test: %s\n", message);
    return condition;
}

WindowLayout complete_layout() {
    WindowLayout layout{};
    layout.outerWidth = 100;
    layout.outerHeight = 60;
    layout.minimumWidth = 24;
    layout.minimumHeight = 28;
    layout.contentsX1 = 10;
    layout.contentsY1 = 8;
    layout.contentsX2 = 90;
    layout.contentsY2 = 52;
    layout.hasFrame = true;
    layout.hasContentsTexture = true;
    layout.frameTextures = {{{10, 12}, {14, 12}, {10, 16}, {14, 16}}};
    layout.contentsTexture = {40, 22};
    layout.parentTransform = identity();
    layout.globalTransform = identity();
    return layout;
}

bool exact_geometry_control() {
    WindowGeometry geometry{};
    const WindowLayout layout = complete_layout();
    if (!check(resolve_window_layout(layout, geometry) == WindowLayoutResult::Visible,
               "complete layout was refused"))
        return false;
    return check(geometry.contentsVisible && geometry.contentsTexture.visible,
                 "contents draws were not visible") &&
           check(geometry.contentsPositions[0] == Vec2{10, 8} &&
                     geometry.contentsPositions[3] == Vec2{90, 52},
                 "contents geometry differs from the retail rectangle") &&
           check(near(geometry.contentsTexture.uv[0].x, -0.5f) &&
                     near(geometry.contentsTexture.uv[0].y, -0.5f) &&
                     near(geometry.contentsTexture.uv[3].x, 1.5f) &&
                     near(geometry.contentsTexture.uv[3].y, 1.5f),
                 "centered contents texture coordinates are wrong") &&
           check(geometry.frame[0].positions[3] == Vec2{10, 12},
                 "top-left corner geometry is wrong") &&
           check(geometry.frame[1].positions[0] == Vec2{86, 0} &&
                     geometry.frame[1].positions[3] == Vec2{100, 12},
                 "top-right corner must use bottom-right width for its origin") &&
           check(geometry.frame[4].positions[0] == Vec2{10, 0} &&
                     geometry.frame[4].positions[3] == Vec2{86, 12},
                 "top edge geometry is wrong") &&
           check(geometry.frame[4].texture == WindowTextureRole::TopRight &&
                     geometry.frame[4].uv[0] == Vec2{0, 0} && geometry.frame[4].uv[3] == Vec2{0, 1},
                 "top edge must sample a column from the top-right texture") &&
           check(geometry.frame[6].texture == WindowTextureRole::BottomLeft &&
                     geometry.frame[6].uv[0] == Vec2{0, 0} && geometry.frame[6].uv[3] == Vec2{1, 0},
                 "left edge must sample a row from the bottom-left texture");
}

bool mirror_known_positive() {
    WindowLayout layout = complete_layout();
    layout.mirror = 0xff;
    WindowGeometry geometry{};
    if (!check(resolve_window_layout(layout, geometry) == WindowLayoutResult::Visible,
               "mirrored layout was refused"))
        return false;
    return check(geometry.frame[0].uv[0] == Vec2{1, 1} && geometry.frame[0].uv[3] == Vec2{0, 0},
                 "corner mirror bits did not reverse both axes") &&
           check(geometry.frame[4].uv[0] == Vec2{1, 1} && geometry.frame[4].uv[3] == Vec2{1, 0},
                 "top edge mirror bits did not reverse the sampled column") &&
           check(geometry.frame[7].uv[0] == Vec2{1, 1} && geometry.frame[7].uv[3] == Vec2{0, 1},
                 "right edge mirror bits did not reverse the sampled row");
}

bool refusal_controls() {
    WindowGeometry geometry{};
    WindowLayout layout = complete_layout();
    layout.outerWidth = 23;
    if (!check(resolve_window_layout(layout, geometry) == WindowLayoutResult::Culled,
               "outer rectangle below the retail minimum was not culled"))
        return false;
    layout = complete_layout();
    layout.frameTextures[2].height = 0;
    return check(resolve_window_layout(layout, geometry) == WindowLayoutResult::Invalid,
                 "zero-height frame texture was not rejected");
}

} // namespace

int main() {
    return exact_geometry_control() && mirror_known_positive() && refusal_controls() ? 0 : 1;
}
