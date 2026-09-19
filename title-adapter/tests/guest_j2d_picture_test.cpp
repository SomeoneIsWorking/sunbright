// Drives the shipping J2D picture reader over a synthetic guest pane.
//
// The whole risk in this reader is silence. Every field it reads is at a fixed offset, and an
// offset that is wrong by four bytes does not produce an obviously broken quad -- it produces a
// plausible one, drawn in the wrong place, with the wrong transparency, out of the wrong image.
// So the positive case states every field's value and the negative cases prove the reader refuses
// rather than filling in: a pane with no area is one `J2DPane::draw` never draws, a texture count
// outside one to four is one `J2DPicture` cannot hold, and a texture the reader cannot follow has
// to be named rather than left as a zero-sized image the decoder would then reject for a reason
// that has nothing to do with what went wrong.

#include <sunbright/title_adapter/guest_j2d_picture.h>

#include "guest_image.h"

#include <cassert>
#include <cstdint>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestPicture;
using sb::title_adapter::GuestPictureError;
using sb::title_adapter::read_guest_matrix;
using sb::title_adapter::read_guest_picture;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress PICTURE = 0x80000000;
constexpr GuestAddress TEXTURE = 0x80000400;
constexpr GuestAddress SECOND_TEXTURE = 0x80000500;
constexpr GuestAddress PALETTE = 0x80000600;
constexpr GuestAddress MATRIX = 0x80000700;

// One `JUTTexture` as `JUTTexture::storeTIMG` leaves it, with no palette.
void write_texture(Image& image, GuestAddress texture, std::uint16_t width, std::uint16_t height,
                   std::uint32_t format, GuestAddress palette) {
    image.word(texture + 0x20, texture + 0x800);
    image.word(texture + 0x24, texture + 0x820);
    image.word(texture + 0x2C, palette);
    image.word(texture + 0x34, format);
    image.word(texture + 0x38, 1);
    image.half(texture + 0x3C, width);
    image.half(texture + 0x3E, height);
    image.byte(texture + 0x40, 1);
    image.byte(texture + 0x41, 2);
    image.byte(texture + 0x42, 4);
    image.byte(texture + 0x43, 1);
}

// A pane the title could have authored: the 456x304 logo quad, opaque, drawn from one texture.
Image logo_pane() {
    Image image;
    image.word(PICTURE + 0x14, 0); // bounds
    image.word(PICTURE + 0x18, 0);
    image.word(PICTURE + 0x1C, 456);
    image.word(PICTURE + 0x20, 304);
    image.word(PICTURE + 0x34, 8); // clip rect
    image.word(PICTURE + 0x38, 12);
    image.word(PICTURE + 0x3C, 440);
    image.word(PICTURE + 0x40, 290);
    for (std::size_t index = 0; index < 12; ++index) {
        image.real(PICTURE + 0x54 + static_cast<GuestAddress>(index * 4),
                   static_cast<float>(index));
        image.real(PICTURE + 0x84 + static_cast<GuestAddress>(index * 4),
                   static_cast<float>(100 + index));
    }
    image.byte(PICTURE + 0xCD, 0x80);
    image.word(PICTURE + 0xEC, TEXTURE);
    image.byte(PICTURE + 0xFC, 1);
    image.word(PICTURE + 0x128, 3);
    image.word(PICTURE + 0x12C, 2);
    image.byte(PICTURE + 0x130, 1);
    image.word(PICTURE + 0x134, 0xFFFFFFFF); // a negative wrap mode, which J2D uses
    image.word(PICTURE + 0x138, 1);
    image.word(PICTURE + 0x13C, 0xFFFFFFFF);
    image.word(PICTURE + 0x140, 0x000000FF);
    image.word(PICTURE + 0x144, 0x11223344);
    image.word(PICTURE + 0x148, 0x55667788);
    image.word(PICTURE + 0x14C, 0x99AABBCC);
    image.word(PICTURE + 0x150, 0xDDEEFF00);
    image.word(PICTURE + 0x154, 0x40608000);
    image.word(PICTURE + 0x158, 0x20304050);
    write_texture(image, TEXTURE, 512, 512, 14, 0);
    return image;
}

void reads_every_field_of_a_pane() {
    Image image = logo_pane();
    GuestMemory memory{read_image, &image};

    GuestPicture picture{};
    assert(read_guest_picture(memory, PICTURE, picture) == GuestPictureError::None);
    assert(picture.address == PICTURE);
    assert(picture.bounds.x1 == 0 && picture.bounds.y1 == 0);
    assert(picture.bounds.width() == 456 && picture.bounds.height() == 304);
    assert(picture.clipRect.x1 == 8 && picture.clipRect.y1 == 12);
    assert(picture.clipRect.width() == 432 && picture.clipRect.height() == 278);
    for (std::size_t index = 0; index < 12; ++index) {
        assert(picture.positionMatrix[index] == static_cast<float>(index));
        assert(picture.globalMatrix[index] == static_cast<float>(100 + index));
    }
    assert(picture.colorAlpha == 0x80);
    assert(picture.textureCount == 1);
    assert(picture.binding == 3);
    assert(picture.mirror == 2);
    assert(picture.flip);
    // A wrap mode is signed: read as unsigned it would be four billion, which the layout resolver
    // would then treat as a wrap rather than as the clamp the title asked for.
    assert(picture.wrapHorizontal == -1);
    assert(picture.wrapVertical == 1);
    assert(picture.white == 0xFFFFFFFF);
    assert(picture.black == 0x000000FF);
    assert(picture.cornerColors[0] == 0x11223344);
    assert(picture.cornerColors[3] == 0xDDEEFF00);
    assert(picture.blendKonstColor == 0x40608000);
    assert(picture.blendKonstAlpha == 0x20304050);

    const auto& texture = picture.textures[0];
    assert(texture.address == TEXTURE);
    assert(texture.resource == TEXTURE + 0x800);
    assert(texture.data == TEXTURE + 0x820);
    assert(texture.format == 14);
    assert(texture.alphaEnabled == 1);
    assert(texture.width == 512 && texture.height == 512);
    assert(texture.wrapS == 1 && texture.wrapT == 2);
    assert(texture.minFilter == 4 && texture.magFilter == 1);
    assert(!texture.palette.present());
    // The layers past the count stay untouched rather than carrying whatever the array held.
    assert(picture.textures[1].address == 0);
}

void follows_an_indexed_texture_to_its_palette() {
    Image image = logo_pane();
    image.byte(PICTURE + 0xFC, 2);
    image.word(PICTURE + 0xF0, SECOND_TEXTURE);
    write_texture(image, SECOND_TEXTURE, 64, 32, 9, PALETTE);
    image.word(PALETTE + 0x10, 1);
    image.word(PALETTE + 0x14, PALETTE + 0x40);
    image.half(PALETTE + 0x18, 256);
    GuestMemory memory{read_image, &image};

    GuestPicture picture{};
    assert(read_guest_picture(memory, PICTURE, picture) == GuestPictureError::None);
    assert(picture.textureCount == 2);
    const auto& palette = picture.textures[1].palette;
    assert(palette.present());
    assert(palette.address == PALETTE);
    assert(palette.format == 1);
    assert(palette.colorTable == PALETTE + 0x40);
    assert(palette.entries == 256);

    // A palette object with no colour table is worse than no palette: the decoder would be handed
    // an indexed image and nothing to index it through.
    image.word(PALETTE + 0x14, 0);
    assert(read_guest_picture(memory, PICTURE, picture) ==
           GuestPictureError::NullPaletteColorTable);
}

void refuses_a_pane_with_no_area() {
    Image image = logo_pane();
    image.word(PICTURE + 0x1C, 0);
    GuestMemory memory{read_image, &image};

    GuestPicture picture{};
    assert(read_guest_picture(memory, PICTURE, picture) == GuestPictureError::EmptyBounds);
}

void refuses_a_texture_count_a_picture_cannot_hold() {
    Image image = logo_pane();
    GuestMemory memory{read_image, &image};
    GuestPicture picture{};

    image.byte(PICTURE + 0xFC, 0);
    assert(read_guest_picture(memory, PICTURE, picture) ==
           GuestPictureError::TextureCountOutOfRange);
    image.byte(PICTURE + 0xFC, 5);
    assert(read_guest_picture(memory, PICTURE, picture) ==
           GuestPictureError::TextureCountOutOfRange);
}

void refuses_what_it_cannot_read() {
    Image image = logo_pane();
    GuestMemory memory{read_image, &image};
    GuestPicture picture{};

    assert(read_guest_picture({nullptr, nullptr}, PICTURE, picture) == GuestPictureError::NoReader);
    assert(read_guest_picture(memory, 0, picture) == GuestPictureError::NullPicture);
    assert(read_guest_picture(memory, 0x70000000, picture) == GuestPictureError::UnreadablePicture);

    image.word(PICTURE + 0xEC, 0);
    assert(read_guest_picture(memory, PICTURE, picture) == GuestPictureError::NullTexture);

    image.word(PICTURE + 0xEC, 0x70000000);
    assert(read_guest_picture(memory, PICTURE, picture) == GuestPictureError::UnreadableTexture);

    image.word(PICTURE + 0xEC, TEXTURE);
    image.word(TEXTURE + 0x2C, 0x70000000);
    assert(read_guest_picture(memory, PICTURE, picture) == GuestPictureError::UnreadablePalette);
}

void leaves_the_output_alone_when_it_refuses() {
    Image image = logo_pane();
    image.word(PICTURE + 0x1C, 0);
    GuestMemory memory{read_image, &image};

    GuestPicture picture{};
    picture.address = 0xDEADBEEF;
    picture.colorAlpha = 0x5A;
    assert(read_guest_picture(memory, PICTURE, picture) == GuestPictureError::EmptyBounds);
    assert(picture.address == 0xDEADBEEF);
    assert(picture.colorAlpha == 0x5A);
}

void reads_the_parent_transform_off_the_guest_stack() {
    Image image = logo_pane();
    for (std::size_t index = 0; index < 12; ++index) {
        image.real(MATRIX + static_cast<GuestAddress>(index * 4), static_cast<float>(index) * 0.5f);
    }
    GuestMemory memory{read_image, &image};

    std::array<float, 12> matrix{};
    assert(read_guest_matrix(memory, MATRIX, matrix));
    for (std::size_t index = 0; index < 12; ++index) {
        assert(matrix[index] == static_cast<float>(index) * 0.5f);
    }
    assert(!read_guest_matrix(memory, 0, matrix));
    assert(!read_guest_matrix(memory, 0x70000000, matrix));
    assert(!read_guest_matrix({nullptr, nullptr}, MATRIX, matrix));
}

} // namespace

int main() {
    reads_every_field_of_a_pane();
    follows_an_indexed_texture_to_its_palette();
    refuses_a_pane_with_no_area();
    refuses_a_texture_count_a_picture_cannot_hold();
    refuses_what_it_cannot_read();
    leaves_the_output_alone_when_it_refuses();
    reads_the_parent_transform_off_the_guest_stack();
    return 0;
}
