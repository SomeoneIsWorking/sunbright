#include <sunbright/native_render/picture_context.h>

#include <cassert>

int main() {
    using sb::native_render::PictureContext;
    using sb::native_render::PictureContextStack;

    PictureContextStack stack{};
    assert(stack.current() == nullptr);
    assert(!stack.pop());

    const auto scissor = sb::native_render::j2d_target_scissor(30, 40, 10, 20);
    assert(scissor.enabled &&
           scissor.space == sb::native_render::ClipCoordinateSpace::TargetPixels);
    assert(scissor.x == 10 && scissor.y == 19 && scissor.width == 20 && scissor.height == 20);
    const auto clippedOut = sb::native_render::j2d_target_scissor(-20, -10, -5, -1);
    assert(clippedOut.enabled && clippedOut.width == 0 && clippedOut.height == 0);
    sb::native_render::PixelRect resolved{};
    const sb::native_render::Canvas scaledCanvas{
        .origin = {0, 0}, .extent = {640, 480}, .viewport = {100, 50, 960, 720}};
    assert(sb::native_render::resolve_scissor(scaledCanvas, scissor, 1280, 960, resolved));
    assert((resolved == sb::native_render::PixelRect{10, 19, 20, 20}));
    assert(!sb::native_render::resolve_scissor(scaledCanvas, clippedOut, 1280, 960, resolved));

    const PictureContext outer{
        {.origin = {0, 0}, .extent = {640, 480}, .viewport = {0, 0, 1280, 960}}, {}, true};
    const PictureContext inner{
        {.origin = {10, 20}, .extent = {100, 50}, .viewport = {40, 60, 400, 200}}, {}, false};
    assert(stack.push(outer));
    assert(stack.current()->canvas == outer.canvas);
    assert(stack.push(inner));
    assert(stack.current()->canvas == inner.canvas);
    assert(stack.depth() == 2);
    assert(stack.pop());
    assert(stack.current()->canvas == outer.canvas);
    assert(stack.pop());
    assert(stack.current() == nullptr);

    PictureContext invalid{};
    assert(!stack.push(invalid));
    for (std::size_t index = 0; index < PictureContextStack::kCapacity; ++index)
        assert(stack.push(outer));
    assert(!stack.push(outer));
}
