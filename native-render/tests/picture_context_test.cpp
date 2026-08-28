#include <sunbright/native_render/picture_context.h>

#include <cassert>

int main() {
    using sb::native_render::PictureContext;
    using sb::native_render::PictureContextStack;

    PictureContextStack stack{};
    assert(stack.current() == nullptr);
    assert(!stack.pop());

    const PictureContext outer{
        {.origin = {0, 0}, .extent = {640, 480}, .viewport = {0, 0, 1280, 960}}, true};
    const PictureContext inner{
        {.origin = {10, 20}, .extent = {100, 50}, .viewport = {40, 60, 400, 200}}, false};
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
