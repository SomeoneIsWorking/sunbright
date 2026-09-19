// Drives the shipping graf-context reader over a synthetic guest object.
//
// The reader's one hard decision is whether an object is a `J2DOrthoGraph` at all, and it has to
// make that decision from the vtable rather than from the `unk4` discriminator `J2DPane::draw`
// itself uses: the base constructor never writes that field in retail, so an object that is not an
// orthographic graph can carry a one there. The case below states exactly that -- a context whose
// class is wrong but whose discriminator says otherwise is refused -- because accepting it would
// publish every pane under it in a logical screen the title never established.

#include <sunbright/title_adapter/guest_j2d_graf_context.h>

#include "guest_image.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace {

using sb::title_adapter::GMSE01_J2D_ORTHO_GRAPH_VTABLE;
using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestGrafContextError;
using sb::title_adapter::GuestGrafContextFill;
using sb::title_adapter::GuestGrafContextVtables;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestOrthoGraph;
using sb::title_adapter::read_guest_graf_context_fill;
using sb::title_adapter::read_guest_ortho_graph;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress CONTEXT = 0x80000000;

// The context `J2DScreen::draw` builds when it is handed none: a 640x480 viewport whose logical
// screen is the same size, with the near and far planes `J2DOrthoGraph` fixes.
Image screen_context() {
    Image image;
    image.word(CONTEXT + 0x00, GMSE01_J2D_ORTHO_GRAPH_VTABLE);
    image.word(CONTEXT + 0x04, 1);
    image.word(CONTEXT + 0x08, 0);
    image.word(CONTEXT + 0x0C, 0);
    image.word(CONTEXT + 0x10, 640);
    image.word(CONTEXT + 0x14, 480);
    image.word(CONTEXT + 0x18, 4);
    image.word(CONTEXT + 0x1C, 8);
    image.word(CONTEXT + 0x20, 636);
    image.word(CONTEXT + 0x24, 472);
    image.word(CONTEXT + 0xD8, 0);
    image.word(CONTEXT + 0xDC, 0);
    image.word(CONTEXT + 0xE0, 640);
    image.word(CONTEXT + 0xE4, 480);
    image.real(CONTEXT + 0xE8, -1.0f);
    image.real(CONTEXT + 0xEC, 1.0f);
    return image;
}

void reads_both_rectangles_a_pane_is_laid_out_between() {
    Image image = screen_context();
    GuestMemory memory{read_image, &image};

    GuestOrthoGraph graph{};
    assert(read_guest_ortho_graph(memory, CONTEXT, {}, graph) == GuestGrafContextError::None);
    assert(graph.address == CONTEXT);
    assert(graph.bounds.width() == 640 && graph.bounds.height() == 480);
    assert(graph.scissorBounds.x1 == 4 && graph.scissorBounds.y1 == 8);
    assert(graph.scissorBounds.width() == 632 && graph.scissorBounds.height() == 464);
    assert(graph.ortho.width() == 640 && graph.ortho.height() == 480);
    assert(graph.nearPlane == -1.0f);
    assert(graph.farPlane == 1.0f);
}

void identifies_the_class_by_its_vtable_and_not_by_a_field() {
    Image image = screen_context();
    // A plain `J2DGrafContext` whose uninitialised discriminator happens to read as orthographic.
    image.word(CONTEXT + 0x00, 0x803E0000);
    GuestMemory memory{read_image, &image};

    GuestOrthoGraph graph{};
    assert(read_guest_ortho_graph(memory, CONTEXT, {}, graph) ==
           GuestGrafContextError::NotOrthographic);
    assert(graph.address == 0);

    // A run against another build names its own vtable rather than being silently wrong about ours.
    const GuestGrafContextVtables other{0x803E0000};
    assert(read_guest_ortho_graph(memory, CONTEXT, other, graph) == GuestGrafContextError::None);
    assert(graph.address == CONTEXT);
}

void refuses_a_screen_or_viewport_with_no_area() {
    Image image = screen_context();
    GuestMemory memory{read_image, &image};
    GuestOrthoGraph graph{};

    image.word(CONTEXT + 0xE0, 0);
    assert(read_guest_ortho_graph(memory, CONTEXT, {}, graph) ==
           GuestGrafContextError::EmptyLogicalScreen);

    image.word(CONTEXT + 0xE0, 640);
    image.word(CONTEXT + 0x14, 0);
    assert(read_guest_ortho_graph(memory, CONTEXT, {}, graph) ==
           GuestGrafContextError::EmptyViewport);
}

// `fillBox` reads four colours and a placement matrix that belong to `J2DGrafContext` itself, so
// they are readable from a context this reader would refuse as a graph. The case proves that: the
// object below carries a vtable that is not `J2DOrthoGraph`'s.
void reads_what_fill_box_paints_with_from_any_context() {
    Image image = screen_context();
    image.word(CONTEXT + 0x00, 0x803E0000);
    image.word(CONTEXT + 0x28, 0x102030FF);
    image.word(CONTEXT + 0x2C, 0x405060FF);
    image.word(CONTEXT + 0x30, 0x708090FF);
    image.word(CONTEXT + 0x34, 0xA0B0C0FF);
    for (std::size_t index = 0; index < 12; ++index) {
        image.real(CONTEXT + 0x84 + static_cast<GuestAddress>(index) * 4,
                   static_cast<float>(index) + 0.5f);
    }
    GuestMemory memory{read_image, &image};

    GuestGrafContextFill fill{};
    assert(read_guest_graf_context_fill(memory, CONTEXT, fill) == GuestGrafContextError::None);
    assert(fill.address == CONTEXT);
    assert(fill.colorTL == 0x102030FF);
    assert(fill.colorTR == 0x405060FF);
    assert(fill.colorBR == 0x708090FF);
    assert(fill.colorBL == 0xA0B0C0FF);
    for (std::size_t index = 0; index < fill.positionMatrix.size(); ++index) {
        assert(fill.positionMatrix[index] == static_cast<float>(index) + 0.5f);
    }
}

void refuses_a_fill_it_cannot_read() {
    Image image = screen_context();
    GuestMemory memory{read_image, &image};
    GuestGrafContextFill fill{};

    assert(read_guest_graf_context_fill({nullptr, nullptr}, CONTEXT, fill) ==
           GuestGrafContextError::NoReader);
    assert(read_guest_graf_context_fill(memory, 0, fill) == GuestGrafContextError::NullContext);
    assert(read_guest_graf_context_fill(memory, 0x70000000, fill) ==
           GuestGrafContextError::UnreadableContext);
    assert(fill.address == 0);
}

void refuses_what_it_cannot_read() {
    Image image = screen_context();
    GuestMemory memory{read_image, &image};
    GuestOrthoGraph graph{};

    assert(read_guest_ortho_graph({nullptr, nullptr}, CONTEXT, {}, graph) ==
           GuestGrafContextError::NoReader);
    assert(read_guest_ortho_graph(memory, 0, {}, graph) == GuestGrafContextError::NullContext);
    assert(read_guest_ortho_graph(memory, 0x70000000, {}, graph) ==
           GuestGrafContextError::UnreadableContext);
}

} // namespace

int main() {
    reads_both_rectangles_a_pane_is_laid_out_between();
    identifies_the_class_by_its_vtable_and_not_by_a_field();
    refuses_a_screen_or_viewport_with_no_area();
    refuses_what_it_cannot_read();
    reads_what_fill_box_paints_with_from_any_context();
    refuses_a_fill_it_cannot_read();
    return 0;
}
