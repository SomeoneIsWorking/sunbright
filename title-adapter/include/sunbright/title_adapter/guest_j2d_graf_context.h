#pragma once

#include <sunbright/title_adapter/guest_j2d_rect.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// Reads the `J2DOrthoGraph` a pane is being drawn under.
//
// Every J2D quad's position is in a logical screen the title chose, and every one of them is
// rasterised into a viewport that is usually a different size. The graf context is where those two
// rectangles are, so without it a pane's bounds are numbers with no space to be in -- and a run
// that assumed 640x480 would be right for the title and wrong for everything the game scales.

// `J2DGrafContext`. The vtable pointer is the first word, as it is for every polymorphic object.
inline constexpr GuestAddress GUEST_GRAF_CONTEXT_VTABLE = 0x00;
inline constexpr GuestAddress GUEST_GRAF_CONTEXT_BOUNDS = 0x08;
inline constexpr GuestAddress GUEST_GRAF_CONTEXT_SCISSOR_BOUNDS = 0x18;

// `J2DOrthoGraph`, which extends it.
inline constexpr GuestAddress GUEST_ORTHO_GRAPH_ORTHO = 0xD8;
inline constexpr GuestAddress GUEST_ORTHO_GRAPH_NEAR = 0xE8;
inline constexpr GuestAddress GUEST_ORTHO_GRAPH_FAR = 0xEC;

// GMSE01's only `J2DOrthoGraph` vtable, recovered from the retail image rather than assumed: it is
// the one run of pointers in the DOL's data sections that carries `J2DOrthoGraph::setPort`
// (0x802ed180), and like every CodeWarrior vtable the object's pointer names its two-word header
// rather than its first slot.
//
// The class field `J2DPane::draw` itself tests -- `J2DGrafContext::unk4 == 1` -- cannot be used
// here. The base constructor never writes it in retail, so a plain `J2DGrafContext` carries
// whatever its storage held, and a reader that trusted it would sometimes read a perspective or
// uninitialised context as an orthographic one and publish quads laid out in a screen that does
// not exist.
inline constexpr GuestAddress GMSE01_J2D_ORTHO_GRAPH_VTABLE = 0x803e14b0;

// A parameter for the same reason the J3D readers take theirs: a run against another build states
// its own address rather than being silently wrong about this one.
struct GuestGrafContextVtables {
    GuestAddress orthoGraph = GMSE01_J2D_ORTHO_GRAPH_VTABLE;
};

enum class GuestGrafContextError : std::uint8_t {
    None,
    NoReader,
    NullContext,
    UnreadableContext,
    NotOrthographic,
    EmptyLogicalScreen,
    EmptyViewport,
};

[[nodiscard]] const char* name(GuestGrafContextError error) noexcept;

struct GuestOrthoGraph {
    GuestAddress address = 0;
    // The viewport, in target pixels.
    GuestRect bounds{};
    GuestRect scissorBounds{};
    // The logical screen a pane's coordinates are in.
    GuestRect ortho{};
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
};

[[nodiscard]] GuestGrafContextError read_guest_ortho_graph(const GuestMemory& memory,
                                                           GuestAddress context,
                                                           const GuestGrafContextVtables& vtables,
                                                           GuestOrthoGraph& out) noexcept;

} // namespace sb::title_adapter
