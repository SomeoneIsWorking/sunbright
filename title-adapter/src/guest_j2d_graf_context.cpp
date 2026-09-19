#include <sunbright/title_adapter/guest_j2d_graf_context.h>

namespace sb::title_adapter {

const char* name(GuestGrafContextError error) noexcept {
    switch (error) {
    case GuestGrafContextError::None:
        return "none";
    case GuestGrafContextError::NoReader:
        return "no_reader";
    case GuestGrafContextError::NullContext:
        return "null_context";
    case GuestGrafContextError::UnreadableContext:
        return "unreadable_context";
    case GuestGrafContextError::NotOrthographic:
        return "not_orthographic";
    case GuestGrafContextError::EmptyLogicalScreen:
        return "empty_logical_screen";
    case GuestGrafContextError::EmptyViewport:
        return "empty_viewport";
    }
    return "unknown";
}

GuestGrafContextError read_guest_ortho_graph(const GuestMemory& memory, GuestAddress context,
                                             const GuestGrafContextVtables& vtables,
                                             GuestOrthoGraph& out) noexcept {
    if (memory.read == nullptr) {
        return GuestGrafContextError::NoReader;
    }
    if (context == 0) {
        return GuestGrafContextError::NullContext;
    }
    const GuestReader reader(memory);

    GuestAddress vtable = 0;
    if (!reader.word(context + GUEST_GRAF_CONTEXT_VTABLE, vtable)) {
        return GuestGrafContextError::UnreadableContext;
    }
    if (vtable != vtables.orthoGraph) {
        return GuestGrafContextError::NotOrthographic;
    }

    GuestOrthoGraph value{};
    value.address = context;
    if (!read_guest_rect(reader, context + GUEST_GRAF_CONTEXT_BOUNDS, value.bounds) ||
        !read_guest_rect(reader, context + GUEST_GRAF_CONTEXT_SCISSOR_BOUNDS,
                         value.scissorBounds) ||
        !read_guest_rect(reader, context + GUEST_ORTHO_GRAPH_ORTHO, value.ortho) ||
        !reader.real(context + GUEST_ORTHO_GRAPH_NEAR, value.nearPlane) ||
        !reader.real(context + GUEST_ORTHO_GRAPH_FAR, value.farPlane)) {
        return GuestGrafContextError::UnreadableContext;
    }

    // Both rectangles are divisors: the logical screen scales a pane's coordinates and the viewport
    // is what they land in. An empty one is not a small screen, it is no screen.
    if (value.ortho.empty()) {
        return GuestGrafContextError::EmptyLogicalScreen;
    }
    if (value.bounds.empty()) {
        return GuestGrafContextError::EmptyViewport;
    }

    out = value;
    return GuestGrafContextError::None;
}

GuestGrafContextError read_guest_graf_context_fill(const GuestMemory& memory, GuestAddress context,
                                                   GuestGrafContextFill& out) noexcept {
    if (memory.read == nullptr) {
        return GuestGrafContextError::NoReader;
    }
    if (context == 0) {
        return GuestGrafContextError::NullContext;
    }
    const GuestReader reader(memory);

    GuestGrafContextFill value{};
    value.address = context;
    if (!reader.word(context + GUEST_GRAF_CONTEXT_COLOR_TL, value.colorTL) ||
        !reader.word(context + GUEST_GRAF_CONTEXT_COLOR_TR, value.colorTR) ||
        !reader.word(context + GUEST_GRAF_CONTEXT_COLOR_BR, value.colorBR) ||
        !reader.word(context + GUEST_GRAF_CONTEXT_COLOR_BL, value.colorBL) ||
        !read_guest_matrix(reader, context + GUEST_GRAF_CONTEXT_POSITION_MATRIX,
                           value.positionMatrix)) {
        return GuestGrafContextError::UnreadableContext;
    }

    out = value;
    return GuestGrafContextError::None;
}

} // namespace sb::title_adapter
