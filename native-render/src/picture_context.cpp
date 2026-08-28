#include <sunbright/native_render/picture_context.h>

namespace sb::native_render {

bool PictureContextStack::push(PictureContext context) noexcept {
    if (!valid(context.canvas) || depth_ == contexts_.size())
        return false;
    contexts_[depth_++] = context;
    return true;
}

bool PictureContextStack::pop() noexcept {
    if (depth_ == 0)
        return false;
    --depth_;
    return true;
}

const PictureContext* PictureContextStack::current() const noexcept {
    return depth_ == 0 ? nullptr : &contexts_[depth_ - 1];
}

std::size_t PictureContextStack::depth() const noexcept {
    return depth_;
}

} // namespace sb::native_render
