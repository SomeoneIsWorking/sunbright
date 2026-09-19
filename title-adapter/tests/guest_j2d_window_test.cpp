// Drives the shipping J2D window reader over a synthetic guest panel.
//
// A window's risk is not that it fails loudly but that it draws something plausible. Its frame is
// four textures in named roles, and reading them in the wrong order mirrors the panel rather than
// breaking it; its `hasFrame` is retail's own four-way test, and getting that wrong invents a
// frame the title never drew or drops one it did. So the positive case states every field, and the
// negative cases prove each refusal is named: a missing corner is a frameless window and not an
// error, a corner the reader cannot follow is an error and not a frameless window, and a palette
// naming no colours is refused rather than passed to a decoder as an empty table.

#include <sunbright/title_adapter/guest_j2d_window.h>

#include "guest_image.h"

#include <cassert>
#include <cstdint>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestWindow;
using sb::title_adapter::GuestWindowError;
using sb::title_adapter::read_guest_window;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;
using sb::title_adapter::test::write_jut_texture;

constexpr GuestAddress WINDOW = 0x80000000;
constexpr GuestAddress TOP_LEFT = 0x80000400;
constexpr GuestAddress TOP_RIGHT = 0x80000480;
constexpr GuestAddress BOTTOM_LEFT = 0x80000500;
constexpr GuestAddress BOTTOM_RIGHT = 0x80000580;
constexpr GuestAddress CONTENTS = 0x80000600;
constexpr GuestAddress PALETTE = 0x80000680;

// A panel the title could have authored: a framed message box, half-faded, over a gradient.
Image message_window() {
    Image image;
    image.word(WINDOW + 0x34, 16); // clip rect
    image.word(WINDOW + 0x38, 24);
    image.word(WINDOW + 0x3C, 608);
    image.word(WINDOW + 0x40, 400);
    for (std::size_t index = 0; index < 12; ++index) {
        image.real(WINDOW + 0x84 + static_cast<GuestAddress>(index * 4),
                   static_cast<float>(100 + index));
    }
    image.byte(WINDOW + 0xCD, 0x80); // mColorAlpha
    image.word(WINDOW + 0x100, TOP_LEFT);
    image.word(WINDOW + 0x104, TOP_RIGHT);
    image.word(WINDOW + 0x108, BOTTOM_LEFT);
    image.word(WINDOW + 0x10C, BOTTOM_RIGHT);
    image.word(WINDOW + 0x110, CONTENTS);
    image.word(WINDOW + 0x114, 0x3C); // mirror flags
    image.word(WINDOW + 0x118, 0x102030FF);
    image.word(WINDOW + 0x11C, 0x405060FF);
    image.word(WINDOW + 0x120, 0x708090FF);
    image.word(WINDOW + 0x124, 0xA0B0C080);
    image.word(WINDOW + 0x128, 0xFFFFFFFF); // frame white
    image.word(WINDOW + 0x12C, 0x00000000); // frame black
    image.word(WINDOW + 0x130, 24);         // minimum width
    image.word(WINDOW + 0x134, 20);         // minimum height
    write_jut_texture(image, TOP_LEFT, 16, 16, 4, 0);
    write_jut_texture(image, TOP_RIGHT, 16, 16, 4, 0);
    write_jut_texture(image, BOTTOM_LEFT, 16, 12, 4, 0);
    write_jut_texture(image, BOTTOM_RIGHT, 16, 12, 4, 0);
    write_jut_texture(image, CONTENTS, 64, 64, 14, 0);
    return image;
}

GuestWindowError read(Image& image, GuestWindow& window) {
    const GuestMemory memory{read_image, &image};
    return read_guest_window(memory, WINDOW, window);
}

void reads_every_field_of_a_framed_window() {
    Image image = message_window();
    GuestWindow window{};
    assert(read(image, window) == GuestWindowError::None);

    assert(window.address == WINDOW);
    assert(window.clipRect.x1 == 16 && window.clipRect.y1 == 24);
    assert(window.clipRect.x2 == 608 && window.clipRect.y2 == 400);
    assert(window.globalMatrix[0] == 100.0f && window.globalMatrix[11] == 111.0f);
    assert(window.colorAlpha == 0x80);
    assert(window.minimumWidth == 24 && window.minimumHeight == 20);
    assert(window.mirror == 0x3C);
    assert(window.frameWhite == 0xFFFFFFFF && window.frameBlack == 0);

    // The four contents colours in the object's order: top-left, top-right, bottom-left,
    // bottom-right. Reading them as a rectangle's corner walk instead would swap the bottom pair.
    assert(window.contentsColors[0] == 0x102030FF);
    assert(window.contentsColors[1] == 0x405060FF);
    assert(window.contentsColors[2] == 0x708090FF);
    assert(window.contentsColors[3] == 0xA0B0C080);

    assert(window.hasFrame);
    assert(window.frameTextures[0].address == TOP_LEFT);
    assert(window.frameTextures[1].address == TOP_RIGHT);
    assert(window.frameTextures[2].address == BOTTOM_LEFT);
    assert(window.frameTextures[3].address == BOTTOM_RIGHT);
    assert(window.frameTextures[0].width == 16 && window.frameTextures[0].height == 16);
    assert(window.frameTextures[2].height == 12);
    assert(window.frameTextures[0].format == 4);

    assert(window.hasContentsTexture);
    assert(window.contentsTexture.address == CONTENTS);
    assert(window.contentsTexture.width == 64 && window.contentsTexture.height == 64);
    assert(!window.contentsTexture.palette.present());
}

// `draw_private` tests all four corners together, so three of them is a window that draws no frame
// at all. A reader that took the three it had would invent a panel retail never drew.
void one_missing_corner_is_a_frameless_window() {
    Image image = message_window();
    image.word(WINDOW + 0x108, 0);
    GuestWindow window{};
    assert(read(image, window) == GuestWindowError::None);
    assert(!window.hasFrame);
    assert(window.frameTextures[0].address == 0);
    assert(window.hasContentsTexture);
}

void a_window_with_no_contents_texture_is_read() {
    Image image = message_window();
    image.word(WINDOW + 0x110, 0);
    GuestWindow window{};
    assert(read(image, window) == GuestWindowError::None);
    assert(window.hasFrame);
    assert(!window.hasContentsTexture);
    assert(window.contentsTexture.address == 0);
}

// A corner that is present but unreadable is the opposite case: the title would have drawn a frame
// here, and answering "no frame" would silently lose it.
void an_unreadable_corner_is_named() {
    Image image = message_window();
    image.word(WINDOW + 0x104, 0x90000000);
    GuestWindow window{};
    assert(read(image, window) == GuestWindowError::UnreadableTexture);
}

void a_palette_naming_no_colors_is_refused() {
    Image image = message_window();
    write_jut_texture(image, CONTENTS, 64, 64, 9, PALETTE);
    image.word(PALETTE + 0x14, 0);
    GuestWindow window{};
    assert(read(image, window) == GuestWindowError::NullPaletteColorTable);
}

void a_null_window_and_a_missing_reader_are_told_apart() {
    Image image = message_window();
    GuestWindow window{};
    const GuestMemory memory{read_image, &image};
    assert(read_guest_window(memory, 0, window) == GuestWindowError::NullWindow);
    const GuestMemory none{};
    assert(read_guest_window(none, WINDOW, window) == GuestWindowError::NoReader);
}

} // namespace

int main() {
    reads_every_field_of_a_framed_window();
    one_missing_corner_is_a_frameless_window();
    a_window_with_no_contents_texture_is_read();
    an_unreadable_corner_is_named();
    a_palette_naming_no_colors_is_refused();
    a_null_window_and_a_missing_reader_are_told_apart();
    return 0;
}
