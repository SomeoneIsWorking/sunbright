#include <sunbright/title_adapter/guest_j2d_window.h>

#include <sunbright/title_adapter/guest_j2d_pane.h>

namespace sb::title_adapter {
namespace {

[[nodiscard]] GuestWindowError translate(GuestJutTextureError error) noexcept {
    switch (error) {
    case GuestJutTextureError::None:
        return GuestWindowError::None;
    case GuestJutTextureError::NullTexture:
    case GuestJutTextureError::UnreadableTexture:
        return GuestWindowError::UnreadableTexture;
    case GuestJutTextureError::UnreadablePalette:
        return GuestWindowError::UnreadablePalette;
    case GuestJutTextureError::NullPaletteColorTable:
        return GuestWindowError::NullPaletteColorTable;
    }
    return GuestWindowError::UnreadableTexture;
}

} // namespace

const char* name(GuestWindowError error) noexcept {
    switch (error) {
    case GuestWindowError::None:
        return "none";
    case GuestWindowError::NoReader:
        return "no_reader";
    case GuestWindowError::NullWindow:
        return "null_window";
    case GuestWindowError::UnreadableWindow:
        return "unreadable_window";
    case GuestWindowError::UnreadableTexture:
        return "unreadable_texture";
    case GuestWindowError::UnreadablePalette:
        return "unreadable_palette";
    case GuestWindowError::NullPaletteColorTable:
        return "null_palette_color_table";
    }
    return "unknown";
}

GuestWindowError read_guest_window(const GuestMemory& memory, GuestAddress window,
                                   GuestWindow& out) noexcept {
    if (memory.read == nullptr) {
        return GuestWindowError::NoReader;
    }
    if (window == 0) {
        return GuestWindowError::NullWindow;
    }
    const GuestReader reader(memory);

    GuestWindow value{};
    value.address = window;
    std::uint32_t minimumWidth = 0;
    std::uint32_t minimumHeight = 0;
    if (!read_guest_rect(reader, window + GUEST_PANE_CLIP_RECT, value.clipRect) ||
        !read_guest_matrix(reader, window + GUEST_PANE_GLOBAL_MATRIX, value.globalMatrix) ||
        !reader.byte(window + GUEST_PANE_COLOR_ALPHA, value.colorAlpha) ||
        !reader.word(window + GUEST_WINDOW_MIRROR, value.mirror) ||
        !reader.word(window + GUEST_WINDOW_FRAME_WHITE, value.frameWhite) ||
        !reader.word(window + GUEST_WINDOW_FRAME_BLACK, value.frameBlack) ||
        !reader.word(window + GUEST_WINDOW_MINIMUM_WIDTH, minimumWidth) ||
        !reader.word(window + GUEST_WINDOW_MINIMUM_HEIGHT, minimumHeight)) {
        return GuestWindowError::UnreadableWindow;
    }
    value.minimumWidth = static_cast<std::int32_t>(minimumWidth);
    value.minimumHeight = static_cast<std::int32_t>(minimumHeight);

    for (std::size_t corner = 0; corner < value.contentsColors.size(); ++corner) {
        if (!reader.word(window + GUEST_WINDOW_CONTENTS_COLORS +
                             static_cast<GuestAddress>(corner * 4),
                         value.contentsColors[corner])) {
            return GuestWindowError::UnreadableWindow;
        }
    }

    // Read all four corner pointers before reading any of the textures behind them: retail's own
    // test is that every one is present, and a window with three is one that draws no frame rather
    // than one this should refuse.
    std::array<GuestAddress, GUEST_WINDOW_FRAME_TEXTURE_COUNT> frame{};
    for (std::size_t corner = 0; corner < frame.size(); ++corner) {
        if (!reader.word(window + GUEST_WINDOW_FRAME_TEXTURES +
                             static_cast<GuestAddress>(corner * 4),
                         frame[corner])) {
            return GuestWindowError::UnreadableWindow;
        }
    }
    value.hasFrame = true;
    for (const GuestAddress corner : frame) {
        value.hasFrame = value.hasFrame && corner != 0;
    }
    if (value.hasFrame) {
        for (std::size_t corner = 0; corner < frame.size(); ++corner) {
            const GuestJutTextureError error =
                read_guest_jut_texture(reader, frame[corner], value.frameTextures[corner]);
            if (error != GuestJutTextureError::None) {
                return translate(error);
            }
        }
    }

    GuestAddress contents = 0;
    if (!reader.word(window + GUEST_WINDOW_CONTENTS_TEXTURE, contents)) {
        return GuestWindowError::UnreadableWindow;
    }
    value.hasContentsTexture = contents != 0;
    if (value.hasContentsTexture) {
        const GuestJutTextureError error =
            read_guest_jut_texture(reader, contents, value.contentsTexture);
        if (error != GuestJutTextureError::None) {
            return translate(error);
        }
    }

    out = value;
    return GuestWindowError::None;
}

} // namespace sb::title_adapter
