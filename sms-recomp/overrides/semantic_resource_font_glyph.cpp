#include "resource_font_glyph_adapter.h"

#include "../frame_interp/populations.h"
#include "../runtime/sb_assert.h"
#include "overrides.h"
#include "semantic_j2d_context.h"

#include <sunbright/native_render/semantic_sink.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

extern "C" void func_802d0b28(CPUState&); // J2DTextBox::draw
extern "C" void func_802d0dd8(CPUState&); // J2DTextBox::drawSelf(int, int, Mtx*)
extern "C" void func_802f178c(CPUState&); // JUTResFont::setGX()
extern "C" void func_802f1864(CPUState&); // JUTResFont::setGX(TColor, TColor)
extern "C" void func_802f1b00(CPUState&); // JUTResFont::drawChar_scale

bool sbr_lerp_enabled();
void sbr_diag_2d_note_text(const char* entry, u32 self);

namespace sb::recomp {
namespace {

struct TextContext {
    native_render::Canvas canvas{};
    native_render::ClipRect clip{};
    native_render::Matrix3x4 transform{};
    std::uint32_t lazyMatrix = 0;
};

std::array<TextContext, 32> g_textContexts{};
std::size_t g_textDepth = 0;
native_render::Color g_fontBlack{};
native_render::Color g_fontWhite{1, 1, 1, 1};
bool g_fontRemapKnown = false;

bool read_matrix(const BigEndianGuestReader& reader, std::uint32_t address,
                 native_render::Matrix3x4& matrix) noexcept {
    for (std::size_t index = 0; index < matrix.value.size(); ++index) {
        if (!reader.f32(address + static_cast<std::uint32_t>(index * sizeof(float)),
                        matrix.value[index])) {
            return false;
        }
    }
    return native_render::valid(matrix);
}

bool read_text_clip(const BigEndianGuestReader& reader, std::uint32_t self, bool enabled,
                    native_render::ClipRect& clip) noexcept {
    clip = {.enabled = enabled};
    if (!enabled)
        return true;
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    if (!reader.s32(self + 0x34, left) || !reader.s32(self + 0x38, top) ||
        !reader.s32(self + 0x3c, right) || !reader.s32(self + 0x40, bottom) || right <= left ||
        bottom <= top) {
        return false;
    }
    clip.x = left;
    clip.y = top;
    clip.width = static_cast<std::uint32_t>(right - left);
    clip.height = static_cast<std::uint32_t>(bottom - top);
    return true;
}

class TextContextScope {
  public:
    TextContextScope(std::uint32_t self, std::uint32_t parentMatrix, bool lazy) {
        if (!native_render::has_semantic_sink())
            return;
        const native_render::PictureContext* pictureContext = current_semantic_j2d_context();
        SB_ASSERT(pictureContext != nullptr,
                  "semantic text draw has no renderer-neutral J2D canvas: textbox=%08x", self);
        SB_ASSERT(g_textDepth < g_textContexts.size(), "semantic text context stack overflow");

        const BigEndianGuestReader reader(live_guest_byte_reader());
        TextContext context{.canvas = pictureContext->canvas};
        SB_ASSERT(read_text_clip(reader, self, pictureContext->clipEnabled, context.clip),
                  "semantic text clip capture failed: textbox=%08x", self);
        if (lazy) {
            context.lazyMatrix = self + 0x54;
        } else {
            native_render::Matrix3x4 parent{};
            native_render::Matrix3x4 global{};
            SB_ASSERT(parentMatrix != 0 && read_matrix(reader, parentMatrix, parent) &&
                          read_matrix(reader, self + 0x84, global),
                      "semantic text transform capture failed: textbox=%08x parent=%08x", self,
                      parentMatrix);
            context.transform = native_render::concatenate_transform(parent, global);
        }
        g_textContexts[g_textDepth++] = context;
        active_ = true;
    }

    ~TextContextScope() {
        if (active_) {
            SB_ASSERT(g_textDepth != 0, "semantic text context stack underflow");
            --g_textDepth;
        }
    }

    TextContextScope(const TextContextScope&) = delete;
    TextContextScope& operator=(const TextContextScope&) = delete;

  private:
    bool active_ = false;
};

bool resolve_current_text_context(TextContext& context) noexcept {
    if (g_textDepth == 0)
        return false;
    context = g_textContexts[g_textDepth - 1U];
    if (context.lazyMatrix == 0)
        return native_render::valid(context.transform);
    return read_matrix(BigEndianGuestReader(live_guest_byte_reader()), context.lazyMatrix,
                       context.transform);
}

class TextPopulationScope {
  public:
    TextPopulationScope() : active_(sbr_lerp_enabled()) {
        if (active_)
            sbr_gxfifo_draw_pop(SB_POP_TEXT);
    }
    ~TextPopulationScope() {
        if (active_)
            sbr_gxfifo_draw_pop(SB_POP_UNLABELLED);
    }

  private:
    bool active_ = false;
};

void run_text_draw(CPUState& cpu) {
    const std::uint32_t self = cpu.gpr[3];
    sbr_diag_2d_note_text("J2DTextBox::draw", self);
    TextContextScope context(self, 0, true);
    func_802d0b28(cpu);
}

void run_text_draw_self(CPUState& cpu) {
    const std::uint32_t self = cpu.gpr[3];
    const std::uint32_t parentMatrix = cpu.gpr[6];
    sbr_diag_2d_note_text("J2DTextBox::drawSelf", self);
    TextContextScope context(self, parentMatrix, false);
    func_802d0dd8(cpu);
}

void run_font_set_gx(CPUState& cpu) {
    func_802f178c(cpu);
    if (native_render::has_semantic_sink()) {
        g_fontBlack = {};
        g_fontWhite = {1, 1, 1, 1};
        g_fontRemapKnown = true;
    }
}

void run_font_set_gx_colors(CPUState& cpu) {
    if (!native_render::has_semantic_sink()) {
        func_802f1864(cpu);
        return;
    }
    const BigEndianGuestReader reader(live_guest_byte_reader());
    std::uint32_t black = 0;
    std::uint32_t white = 0;
    SB_ASSERT(reader.u32(cpu.gpr[4], black) && reader.u32(cpu.gpr[5], white),
              "semantic font remap capture failed: black=%08x white=%08x", cpu.gpr[4], cpu.gpr[5]);
    func_802f1864(cpu);
    g_fontBlack = native_render::color_from_rgba8(black);
    g_fontWhite = native_render::color_from_rgba8(white);
    g_fontRemapKnown = true;
}

void run_font_glyph(CPUState& cpu) {
    const std::uint32_t self = cpu.gpr[3];
    const ResourceGlyphArgs args{.positionX = static_cast<float>(cpu.fpr[1].ps0),
                                 .positionY = static_cast<float>(cpu.fpr[2].ps0),
                                 .scaleX = static_cast<float>(cpu.fpr[3].ps0),
                                 .scaleY = static_cast<float>(cpu.fpr[4].ps0),
                                 .code = cpu.gpr[4],
                                 .applyBearing = cpu.gpr[5] != 0};
    TextPopulationScope population{};
    func_802f1b00(cpu);
    if (!native_render::has_semantic_sink())
        return;

    TextContext context{};
    SB_ASSERT(g_fontRemapKnown, "semantic glyph has no preceding JUTResFont::setGX state");
    SB_ASSERT(resolve_current_text_context(context),
              "semantic glyph has no high-level J2DTextBox transform: font=%08x code=%08x", self,
              args.code);
    CapturedGlyph capture{};
    SB_ASSERT(capture_resource_font_glyph(live_guest_byte_reader(), self, args, context.transform,
                                          g_fontBlack, g_fontWhite, capture),
              "semantic resource-font glyph capture failed: font=%08x code=%08x", self, args.code);
    capture.command.clip = context.clip;
    const native_render::GlyphDraw draw{context.canvas, capture.command};
    SB_ASSERT(native_render::submit_glyph(draw, std::span(&capture.image, 1)),
              "semantic resource-font glyph submission failed: font=%08x code=%08x", self,
              args.code);
}

} // namespace
} // namespace sb::recomp

namespace {
void override_text_draw(CPUState& cpu) {
    sb::recomp::run_text_draw(cpu);
}
void override_text_draw_self(CPUState& cpu) {
    sb::recomp::run_text_draw_self(cpu);
}
void override_font_set_gx(CPUState& cpu) {
    sb::recomp::run_font_set_gx(cpu);
}
void override_font_set_gx_colors(CPUState& cpu) {
    sb::recomp::run_font_set_gx_colors(cpu);
}
void override_font_glyph(CPUState& cpu) {
    sb::recomp::run_font_glyph(cpu);
}
} // namespace

SB_OVERRIDE(0x802d0b28u, override_text_draw, "J2DTextBox::draw",
            "publish semantic text context around retained body")
SB_OVERRIDE(0x802d0dd8u, override_text_draw_self, "J2DTextBox::drawSelf(Mtx)",
            "publish semantic text context around retained body")
SB_OVERRIDE(0x802f178cu, override_font_set_gx, "JUTResFont::setGX",
            "track high-level default font remap and retain original body")
SB_OVERRIDE(0x802f1864u, override_font_set_gx_colors, "JUTResFont::setGX(colors)",
            "track high-level font remap and retain original body")
SB_OVERRIDE(0x802f1b00u, override_font_glyph, "JUTResFont::drawChar_scale",
            "publish semantic resource-font glyph and retain original body")
