#include <sunbright/title_adapter/guest_scrn_fader.h>

namespace sb::title_adapter {

std::int32_t wipe_box_inset(std::int32_t extent, std::uint8_t alpha) noexcept {
    const float scaled = static_cast<float>(alpha) * static_cast<float>(extent >> 1) / 255.0F;
    return static_cast<std::int32_t>(scaled);
}

std::size_t resolve_wipe_box_bands(const GuestRect& frame, std::uint8_t alpha,
                                   std::array<GuestRect, GUEST_WIPE_BOX_BANDS>& bands) noexcept {
    const std::int32_t horizontal = wipe_box_inset(frame.width(), alpha);
    const std::int32_t vertical = wipe_box_inset(frame.height(), alpha);
    const GuestRect inner{frame.x1 + horizontal, frame.y1 + vertical, frame.x2 - horizontal,
                          frame.y2 - vertical};

    // The four quads in the order `draw_wipe_box` pushes them. Each is read off its own vertices
    // rather than derived from a border width, because the guest mixes the frame's edges with the
    // inner rectangle's asymmetrically: the top band stops at the inner right edge, the right band
    // at the inner bottom, and so on, which is what makes the iris rotate as it closes.
    bands[0] = {frame.x1, frame.y1, inner.x2, inner.y1};
    bands[1] = {inner.x2, frame.y1, frame.x2, inner.y2};
    bands[2] = {inner.x1, inner.y2, frame.x2, frame.y2};
    bands[3] = {frame.x1, inner.y1, inner.x1, frame.y2};

    std::size_t drawn = 0;
    for (const GuestRect& band : bands) {
        if (!band.empty()) {
            drawn += 1;
        }
    }
    return drawn;
}

} // namespace sb::title_adapter
