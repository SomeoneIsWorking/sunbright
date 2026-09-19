// SPDX-License-Identifier: GPL-2.0-or-later
#include "boot_options.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace sunbright::gcnport_boot {

bool ParseGuestAddress(const char* text, char** end, u32& address) {
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, end, 16);
    if (*end == text || parsed > 0xffffffffull || errno == ERANGE) {
        return false;
    }
    address = static_cast<u32>(parsed);
    return true;
}

namespace {

constexpr u64 DEFAULT_PROBE_REPORTS = 8;

// Every probe flag takes its address the same way: a guest address, optionally followed by how many
// of that probe's entries to print in full. Six flags each spelled that out, which was six chances
// for one of them to accept a value the others refuse -- and the refusals are the point, since a
// probe installed at an address that parsed differently reports about somewhere else entirely.
[[nodiscard]] bool parse_probe_address(std::string_view flag, const char* text, u32& address,
                                       u64& reports) {
    char* end = nullptr;
    if (!ParseGuestAddress(text, &end, address) || (*end != '\0' && *end != ':')) {
        std::fprintf(stderr, "gmse01_boot: %.*s needs <hex-addr>[:<reports>], got '%s'\n",
                     static_cast<int>(flag.size()), flag.data(), text);
        return false;
    }
    reports = DEFAULT_PROBE_REPORTS;
    if (*end != ':') {
        return true;
    }
    const char* const reports_text = end + 1;
    errno = 0;
    const unsigned long long parsed = std::strtoull(reports_text, &end, 0);
    if (end == reports_text || *end != '\0' || errno == ERANGE) {
        std::fprintf(stderr, "gmse01_boot: %.*s report count must be an integer, got '%s'\n",
                     static_cast<int>(flag.size()), flag.data(), reports_text);
        return false;
    }
    reports = parsed;
    return true;
}

} // namespace

bool parse_boot_options(int argc, char** argv, BootRequest& request) {
    const auto usage = [argv]() {
        std::fprintf(stderr,
                     "usage: %s <path-to-extracted-main.dol> [--disc <disc-image>] "
                     "[--max-blocks <n>] [--raw-faults] [--dump-guest <hex-addr>[:<words>] ...] "
                     "[--count-calls <hex-addr> ...] [--watch-guest <hex-addr>[:<words>] ...] "
                     "[--super-call <hex-addr>:<round-trips>:<instruction-budget> ...] "
                     "[--read-shapes <hex-addr>[:<reports>]] [--j3d-sys <hex-addr>]\n"
                     "[--read-materials <hex-addr>[:<reports>]] "
                     "[--read-lighting <hex-addr>[:<reports>] ...] "
                     "[--read-models <hex-addr>[:<reports>]] "
                     "[--read-j2d-screen <hex-addr>[:<reports>]] "
                     "[--read-pictures <hex-addr>[:<reports>]] "
                     "[--read-windows <hex-addr>[:<reports>]] "
                     "[--read-projections <hex-addr>[:<reports>]] "
                     "[--read-efb texture|display|source|clear:<hex-addr>[:<reports>] ...] "
                     "[--read-viewport viewport|scissor:<hex-addr>[:<reports>] ...] "
                     "[--read-rectangles fade|wipe|fillbox:<hex-addr>[:<reports>] ...] "
                     "[--read-matrices load|current:<hex-addr>[:<reports>] ...] "
                     "[--read-glyphs setgx|remap|glyph:<hex-addr>[:<reports>] ...] "
                     "[--press-buttons <hex-addr>[:<reports>]] "
                     "[--pad-script \"<frame>:<buttons> ...\"] "
                     "[--render-frames <hex-addr>] [--dump-frame <path>] "
                     "[--dump-frame-index <n>] [--draw-mode normal|family-map|opaque] "
                     "[--draw-skip <n>] [--draw-limit <n>] [--draw-log-frame <n>]\n",
                     argv[0]);
    };
    if (argc < 2 || argv[1][0] == '-') {
        usage();
        return false;
    }

    // Optional overrides. Every one of these refuses a malformed value rather than silently falling
    // back to its default: a run that quietly used a different budget, or quietly mounted no disc,
    // would be indistinguishable from one that genuinely reached a different boundary.
    for (int argument = 2; argument < argc; ++argument) {
        const std::string_view name = argv[argument];
        // The one flag that takes no value. Handled before the value check below, which would
        // otherwise reject it for having nothing after it.
        if (name == "--raw-faults") {
            request.report_counters_on_fault = false;
            continue;
        }

        if (argument + 1 >= argc) {
            std::fprintf(stderr, "gmse01_boot: %s needs a value\n", argv[argument]);
            return false;
        }
        const char* const value = argv[++argument];

        if (name == "--disc") {
            request.disc_image_path = value;
        } else if (name == "--max-blocks") {
            char* end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(value, &end, 0);
            // strtoull saturates at ULLONG_MAX on overflow, so an out-of-range argument would
            // otherwise be accepted as a huge budget rather than refused; errno is the only way to
            // tell them apart.
            if (end == value || *end != '\0' || parsed == 0 || errno == ERANGE) {
                std::fprintf(stderr,
                             "gmse01_boot: --max-blocks must be a positive value, got '%s'\n",
                             value);
                return false;
            }
            request.block_budget = parsed;
        } else if (name == "--dump-guest") {
            // <hex-addr>[:<words>]. A default window is one cache line, which is enough to
            // recognise an object header and its first members without hiding a typo in a wall of
            // zeroes.
            constexpr u32 DEFAULT_WORDS = 8;
            constexpr u32 MAX_WORDS = 4096;
            GuestMemoryWindow window;
            char* end = nullptr;
            if (!ParseGuestAddress(value, &end, window.address) || (*end != '\0' && *end != ':')) {
                std::fprintf(stderr,
                             "gmse01_boot: --dump-guest needs <hex-addr>[:<words>], got '%s'\n",
                             value);
                return false;
            }
            window.words = DEFAULT_WORDS;
            if (*end == ':') {
                const char* const words_text = end + 1;
                errno = 0;
                const unsigned long long parsed_words = std::strtoull(words_text, &end, 0);
                if (end == words_text || *end != '\0' || parsed_words == 0 ||
                    parsed_words > MAX_WORDS || errno == ERANGE) {
                    std::fprintf(stderr,
                                 "gmse01_boot: --dump-guest word count must be 1..%u, got '%s'\n",
                                 MAX_WORDS, words_text);
                    return false;
                }
                window.words = static_cast<u32>(parsed_words);
            }
            request.dump_windows.push_back(window);
        } else if (name == "--count-calls") {
            u32 address = 0;
            char* end = nullptr;
            if (!ParseGuestAddress(value, &end, address) || *end != '\0') {
                std::fprintf(stderr, "gmse01_boot: --count-calls needs <hex-addr>, got '%s'\n",
                             value);
                return false;
            }
            request.counted_call_addresses.push_back(address);
        } else if (name == "--watch-guest") {
            // <hex-addr>[:<words>]. Kept small on purpose: this window is re-read and compared at
            // every batch report, and a wide one turns a change report into a wall of text in which
            // the word that actually moved is the hard part to find.
            constexpr u32 DEFAULT_WATCH_WORDS = 4;
            constexpr u32 MAX_WATCH_WORDS = 64;
            GuestWatch watch;
            char* end = nullptr;
            if (!ParseGuestAddress(value, &end, watch.address) || (*end != '\0' && *end != ':')) {
                std::fprintf(stderr,
                             "gmse01_boot: --watch-guest needs <hex-addr>[:<words>], got '%s'\n",
                             value);
                return false;
            }
            watch.words = DEFAULT_WATCH_WORDS;
            if (*end == ':') {
                const char* const words_text = end + 1;
                errno = 0;
                const unsigned long long parsed_words = std::strtoull(words_text, &end, 0);
                if (end == words_text || *end != '\0' || parsed_words == 0 ||
                    parsed_words > MAX_WATCH_WORDS || errno == ERANGE) {
                    std::fprintf(stderr,
                                 "gmse01_boot: --watch-guest word count must be 1..%u, got '%s'\n",
                                 MAX_WATCH_WORDS, words_text);
                    return false;
                }
                watch.words = static_cast<u32>(parsed_words);
            }
            constexpr u32 GUEST_RAM_START = 0x80000000;
            constexpr u32 GUEST_RAM_END = 0x81800000;
            if (watch.address < GUEST_RAM_START || watch.address % 4 != 0 ||
                watch.address + watch.words * 4 > GUEST_RAM_END) {
                std::fprintf(stderr,
                             "gmse01_boot: --watch-guest 0x%08x is not a word-aligned guest RAM "
                             "window of %u word(s)\n",
                             watch.address, watch.words);
                return false;
            }
            request.guest_watches.push_back(watch);
        } else if (name == "--read-shapes") {
            // <hex-addr>[:<reports>]. The address is J3DShape::draw (0x802e0390 in GMSE01); the
            // count bounds only how many are printed in full, never how many are read.
            u32 address = 0;
            if (!parse_probe_address(name, value, address, request.shape_probe_reports)) {
                return false;
            }
            request.shape_probe_addresses.push_back(address);
        } else if (name == "--read-materials") {
            // <hex-addr>[:<reports>]. The address is J3DMatPacket::draw (0x802edc38 in GMSE01),
            // which is where the title resolves the material its shape packets are drawn with. The
            // count bounds only how many are printed in full, never how many are read.
            u32 address = 0;
            if (!parse_probe_address(name, value, address, request.material_probe_reports)) {
                return false;
            }
            request.material_probe_addresses.push_back(address);
        } else if (name == "--read-j2d-screen") {
            // <hex-addr>[:<reports>]. The address is J2DGrafContext::setup2D (0x802eb6bc in
            // GMSE01), where the title establishes the logical screen and viewport its 2D is drawn
            // in. Without it a picture has a size and no space to be in, so --read-pictures on its
            // own reports panes it deliberately did not publish.
            u32 address = 0;
            if (!parse_probe_address(name, value, address, request.j2d_context_probe_reports)) {
                return false;
            }
            request.j2d_context_probe_addresses.push_back(address);
        } else if (name == "--read-pictures") {
            // <hex-addr>[:<reports>]. The address is J2DPicture::drawSelf(int, int, Mtx*)
            // (0x802cc7c0 in GMSE01), reached from J2DPane::draw once the pane's transform, clip
            // and inherited opacity are final and the parent transform is the argument in hand.
            u32 address = 0;
            if (!parse_probe_address(name, value, address, request.picture_probe_reports)) {
                return false;
            }
            request.picture_probe_addresses.push_back(address);
        } else if (name == "--read-windows") {
            // <hex-addr>[:<reports>]. The address is J2DWindow::draw_private (0x802d18ec in
            // GMSE01), the one point both drawSelf overloads funnel into and the only place the
            // outer rectangle, the contents rectangle and the parent transform are all arguments
            // in hand -- drawSelf(int, int) builds its matrix on its own stack.
            u32 address = 0;
            if (!parse_probe_address(name, value, address, request.window_probe_reports)) {
                return false;
            }
            request.window_probe_addresses.push_back(address);
        } else if (name == "--read-models") {
            // <hex-addr>[:<reports>]. The address is J3DShape::draw (0x802e0390 in GMSE01), where
            // the shape and the material packet in force are both in hand. The count bounds only
            // how many draws are printed in full, never how many are composed.
            u32 address = 0;
            if (!parse_probe_address(name, value, address, request.model_probe_reports)) {
                return false;
            }
            request.model_probe_addresses.push_back(address);
        } else if (name == "--dump-frame") {
            // Where to write the one sampled frame, as a P6 PPM. Useless without --render-frames,
            // and refused rather than ignored: a run that was asked for an image and silently took
            // no picture is indistinguishable from one whose renderer drew nothing.
            request.frame_image_path = value;
        } else if (name == "--draw-log-frame") {
            // Lists every draw of one frame with its family and policy, so a draw ordinal found by
            // bounding the frame can be named.
            errno = 0;
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(value, &end, 0);
            if (end == value || *end != '\0' || errno == ERANGE || parsed == 0) {
                std::fprintf(stderr,
                             "gmse01_boot: --draw-log-frame needs a frame number from 1, got "
                             "'%s'\n",
                             value);
                return false;
            }
            request.draw_log_frame = parsed;
        } else if (name == "--draw-skip") {
            // How many of each frame's leading draws are withheld. With a limit it isolates a
            // range: bounding the prefix names the draw that introduces a defect, and dropping the
            // prefix as well is what shows that draw's own geometry and colour on an empty frame.
            errno = 0;
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(value, &end, 0);
            if (end == value || *end != '\0' || errno == ERANGE || parsed == 0) {
                std::fprintf(stderr,
                             "gmse01_boot: --draw-skip needs a draw count from 1, got '%s'\n",
                             value);
                return false;
            }
            request.draw_skip = parsed;
        } else if (name == "--draw-limit") {
            // How many of each frame's draws reach the sink. Bounding it and moving the bound is
            // how a defect somewhere in a stack of blended draws is attributed to one of them.
            errno = 0;
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(value, &end, 0);
            if (end == value || *end != '\0' || errno == ERANGE || parsed == 0) {
                std::fprintf(stderr,
                             "gmse01_boot: --draw-limit needs a draw count from 1, got '%s'\n",
                             value);
                return false;
            }
            request.draw_limit = parsed;
        } else if (name == "--draw-mode") {
            // What the publisher does to each draw's material. The two diagnostic modes change what
            // is drawn, so a run using one says so in its report rather than producing an image
            // that could be mistaken for a rendering.
            if (!parse_draw_diagnostic_mode(value, request.draw_mode)) {
                std::fprintf(stderr,
                             "gmse01_boot: --draw-mode takes normal, family-map or opaque, got "
                             "'%s'\n",
                             value);
                return false;
            }
        } else if (name == "--dump-frame-index") {
            // Which sealed frame --dump-frame writes. Omitted, the first frame with any content is
            // written and only that one is ever downloaded; named, every frame up to it is.
            errno = 0;
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(value, &end, 0);
            if (end == value || *end != '\0' || errno == ERANGE || parsed == 0) {
                std::fprintf(stderr,
                             "gmse01_boot: --dump-frame-index needs a frame number from 1, got "
                             "'%s'\n",
                             value);
                return false;
            }
            request.frame_image_index = parsed;
        } else if (name == "--render-frames") {
            // <hex-addr>. The address is the title's frame seam -- GMSE01 reaches one in
            // JDrama::TVideo::waitForRetrace (0x802fc9a4). Unlike the probes, this takes no report
            // count: it does not print per frame, it renders every one of them.
            u32 address = 0;
            char* end = nullptr;
            if (!ParseGuestAddress(value, &end, address) || *end != '\0') {
                std::fprintf(stderr, "gmse01_boot: --render-frames needs <hex-addr>, got '%s'\n",
                             value);
                return false;
            }
            request.frame_seam_addresses.push_back(address);
        } else if (name == "--read-projections") {
            // <hex-addr>[:<reports>]. The address is GXSetProjection (0x80362c34 in GMSE01), which
            // takes the matrix in r3 and the projection type in r4. The count bounds only how many
            // distinct projections are printed, never how many are read or published.
            u32 address = 0;
            if (!parse_probe_address(name, value, address, request.projection_probe_reports)) {
                return false;
            }
            request.projection_probe_addresses.push_back(address);
        } else if (name == "--read-efb") {
            // <kind>:<hex-addr>[:<reports>], with kind one of texture, display, source, clear.
            // In GMSE01 those are GXCopyTex 0x8035ee5c, GXCopyDisp 0x8035ecec, GXSetTexCopySrc
            // 0x8035e388 and GXSetCopyClear 0x8035ea40. The kind is named rather than inferred
            // from the address: the four entries take different arguments, and guessing wrong
            // would report a width as a clear flag without ever saying so.
            const std::string_view text(value);
            const std::size_t separator = text.find(':');
            if (separator == std::string_view::npos) {
                std::fprintf(stderr,
                             "gmse01_boot: --read-efb needs <kind>:<hex-addr>[:<reports>], "
                             "got '%s'\n",
                             value);
                return false;
            }
            const std::string_view kind = text.substr(0, separator);
            EfbCopyProbeRequest probe;
            if (kind == "texture") {
                probe.entry = GuestEfbCopyProbe::Entry::CopyToTexture;
            } else if (kind == "display") {
                probe.entry = GuestEfbCopyProbe::Entry::CopyToDisplay;
            } else if (kind == "source") {
                probe.entry = GuestEfbCopyProbe::Entry::SetTextureSource;
            } else if (kind == "clear") {
                probe.entry = GuestEfbCopyProbe::Entry::SetClear;
            } else {
                std::fprintf(stderr,
                             "gmse01_boot: --read-efb kind must be texture, display, source or "
                             "clear, got '%.*s'\n",
                             static_cast<int>(kind.size()), kind.data());
                return false;
            }
            if (!parse_probe_address(name, value + separator + 1, probe.address,
                                     request.efb_copy_probe_reports)) {
                return false;
            }
            request.efb_copy_probes.push_back(probe);
        } else if (name == "--read-viewport") {
            // <kind>:<hex-addr>[:<reports>], with kind one of viewport or scissor. In GMSE01 those
            // are GXSetViewportJitter 0x80362fac -- the single owner, which GXSetViewport at
            // 0x803630c8 delegates to, so hooking it sees both routes once each -- and GXSetScissor
            // 0x80363138.
            const std::string_view text(value);
            const std::size_t separator = text.find(':');
            if (separator == std::string_view::npos) {
                std::fprintf(stderr,
                             "gmse01_boot: --read-viewport needs <kind>:<hex-addr>[:<reports>], "
                             "got '%s'\n",
                             value);
                return false;
            }
            const std::string_view kind = text.substr(0, separator);
            ViewportProbeRequest probe;
            if (kind == "viewport") {
                probe.entry = GuestViewportProbe::Entry::Viewport;
            } else if (kind == "scissor") {
                probe.entry = GuestViewportProbe::Entry::Scissor;
            } else {
                std::fprintf(stderr,
                             "gmse01_boot: --read-viewport kind must be viewport or scissor, "
                             "got '%.*s'\n",
                             static_cast<int>(kind.size()), kind.data());
                return false;
            }
            if (!parse_probe_address(name, value + separator + 1, probe.address,
                                     request.viewport_probe_reports)) {
                return false;
            }
            request.viewport_probes.push_back(probe);
        } else if (name == "--read-rectangles") {
            // <kind>:<hex-addr>[:<reports>], with kind one of fade, wipe or fillbox. In GMSE01
            // those are ScrnFader's fill_rect 0x80140390 and draw_wipe_box 0x801400cc -- the two
            // quads TSMSFader::draw chooses between -- and J2DGrafContext::fillBox 0x802eba70.
            // The fader pair needs --read-projections and --read-viewport viewport to have a
            // screen to be drawn in; fillbox needs --read-j2d-screen.
            const std::string_view text(value);
            const std::size_t separator = text.find(':');
            if (separator == std::string_view::npos) {
                std::fprintf(stderr,
                             "gmse01_boot: --read-rectangles needs "
                             "<kind>:<hex-addr>[:<reports>], got '%s'\n",
                             value);
                return false;
            }
            const std::string_view kind = text.substr(0, separator);
            SolidRectangleProbeRequest probe;
            if (kind == "fade") {
                probe.entry = GuestSolidRectangleProbe::Entry::FadeRect;
            } else if (kind == "wipe") {
                probe.entry = GuestSolidRectangleProbe::Entry::WipeBox;
            } else if (kind == "fillbox") {
                probe.entry = GuestSolidRectangleProbe::Entry::FillBox;
            } else {
                std::fprintf(stderr,
                             "gmse01_boot: --read-rectangles kind must be fade, wipe or fillbox, "
                             "got '%.*s'\n",
                             static_cast<int>(kind.size()), kind.data());
                return false;
            }
            if (!parse_probe_address(name, value + separator + 1, probe.address,
                                     request.solid_rectangle_probe_reports)) {
                return false;
            }
            request.solid_rectangle_probes.push_back(probe);
        } else if (name == "--read-matrices") {
            // <kind>:<hex-addr>[:<reports>], with kind one of load or current. In GMSE01 those are
            // GXLoadPosMtxImm 0x80362e0c and GXSetCurrentMtx 0x80362eec. Both are needed before an
            // immediate-mode draw has a placement: the first says what a row holds and the second
            // says which row a vertex is multiplied by.
            const std::string_view text(value);
            const std::size_t separator = text.find(':');
            if (separator == std::string_view::npos) {
                std::fprintf(stderr,
                             "gmse01_boot: --read-matrices needs <kind>:<hex-addr>[:<reports>], "
                             "got '%s'\n",
                             value);
                return false;
            }
            const std::string_view kind = text.substr(0, separator);
            MatrixProbeRequest probe;
            if (kind == "load") {
                probe.entry = GuestMatrixProbe::Entry::LoadPosMtxImm;
            } else if (kind == "current") {
                probe.entry = GuestMatrixProbe::Entry::SetCurrentMtx;
            } else {
                std::fprintf(stderr,
                             "gmse01_boot: --read-matrices kind must be load or current, got "
                             "'%.*s'\n",
                             static_cast<int>(kind.size()), kind.data());
                return false;
            }
            if (!parse_probe_address(name, value + separator + 1, probe.address,
                                     request.matrix_probe_reports)) {
                return false;
            }
            request.matrix_probes.push_back(probe);
        } else if (name == "--read-glyphs") {
            // <kind>:<hex-addr>[:<reports>], with kind one of setgx, remap or glyph. In GMSE01
            // those are JUTResFont::setGX() 0x802f178c, its two-colour overload 0x802f1864, and
            // drawChar_scale 0x802f1b00. A glyph needs all three plus --read-projections,
            // --read-viewport viewport and --read-matrices: the font says what its ramp means, the
            // matrices say where its quad is, and the projection pair says what that means on
            // screen.
            const std::string_view text(value);
            const std::size_t separator = text.find(':');
            if (separator == std::string_view::npos) {
                std::fprintf(stderr,
                             "gmse01_boot: --read-glyphs needs <kind>:<hex-addr>[:<reports>], got "
                             "'%s'\n",
                             value);
                return false;
            }
            const std::string_view kind = text.substr(0, separator);
            GlyphProbeRequest probe;
            if (kind == "setgx") {
                probe.entry = GuestGlyphProbe::Entry::SetGXDefault;
            } else if (kind == "remap") {
                probe.entry = GuestGlyphProbe::Entry::SetGXRemap;
            } else if (kind == "glyph") {
                probe.entry = GuestGlyphProbe::Entry::DrawChar;
            } else {
                std::fprintf(stderr,
                             "gmse01_boot: --read-glyphs kind must be setgx, remap or glyph, got "
                             "'%.*s'\n",
                             static_cast<int>(kind.size()), kind.data());
                return false;
            }
            if (!parse_probe_address(name, value + separator + 1, probe.address,
                                     request.glyph_probe_reports)) {
                return false;
            }
            request.glyph_probes.push_back(probe);
        } else if (name == "--press-buttons") {
            // PADRead in GMSE01 is 0x80351600. The original still runs; what this changes is what
            // port 0 contains by the time JUTGamePad::read walks the array.
            if (!parse_probe_address(name, value, request.pad_read_address,
                                     request.pad_probe_reports)) {
                return false;
            }
        } else if (name == "--pad-script") {
            const sb::title_adapter::GuestPadScriptError error = request.pad_script.parse(value);
            if (error != sb::title_adapter::GuestPadScriptError::None) {
                std::fprintf(stderr, "gmse01_boot: --pad-script is not a script (%s): '%s'\n",
                             sb::title_adapter::name(error), value);
                return false;
            }
        } else if (name == "--read-lighting") {
            // <hex-addr>[:<reports>]. The address is TLightCommon::setLight (0x80229a30 in GMSE01)
            // or its TLightMario override (0x80229610); pass the flag twice to cover both. The
            // count bounds only how many relights are printed in full, never how many are read.
            u32 address = 0;
            if (!parse_probe_address(name, value, address, request.lighting_probe_reports)) {
                return false;
            }
            request.lighting_probe_addresses.push_back(address);
        } else if (name == "--j3d-sys") {
            // The guest address of j3dSys, for a build whose globals sit elsewhere. Defaults to
            // GMSE01's, which title-adapter derives from J3DShape::loadVtxArray's own reads.
            char* end = nullptr;
            if (!ParseGuestAddress(value, &end, request.shape_probe_system) || *end != '\0') {
                std::fprintf(stderr, "gmse01_boot: --j3d-sys needs <hex-addr>, got '%s'\n", value);
                return false;
            }
        } else if (name == "--super-call") {
            // <hex-addr>:<round-trips>:<instruction-budget>. All three are stated rather than
            // defaulted: how many entries to route through the interpreter and how long the callee
            // is allowed to be are properties of the function being called, and a wrong guess at
            // either is a hard fault or an unmeasured run rather than a smaller answer.
            SuperCall super;
            char* end = nullptr;
            if (!ParseGuestAddress(value, &end, super.address) || *end != ':') {
                std::fprintf(stderr,
                             "gmse01_boot: --super-call needs "
                             "<hex-addr>:<round-trips>:<instruction-budget>, got '%s'\n",
                             value);
                return false;
            }
            const char* const round_trips_text = end + 1;
            errno = 0;
            const unsigned long long parsed_round_trips = std::strtoull(round_trips_text, &end, 0);
            if (end == round_trips_text || *end != ':' || parsed_round_trips == 0 ||
                errno == ERANGE) {
                std::fprintf(stderr,
                             "gmse01_boot: --super-call round-trip count must be a positive "
                             "integer, got '%s'\n",
                             round_trips_text);
                return false;
            }
            super.max_round_trips = parsed_round_trips;
            const char* const budget_text = end + 1;
            errno = 0;
            const unsigned long long parsed_budget = std::strtoull(budget_text, &end, 0);
            if (end == budget_text || *end != '\0' || parsed_budget == 0 ||
                parsed_budget > 0xffffffffull || errno == ERANGE) {
                std::fprintf(stderr,
                             "gmse01_boot: --super-call instruction budget must be 1..2^32-1, "
                             "got '%s'\n",
                             budget_text);
                return false;
            }
            super.instruction_budget = static_cast<u32>(parsed_budget);
            request.super_calls.push_back(super);
        } else {
            std::fprintf(stderr, "gmse01_boot: unknown option '%s'\n", argv[argument - 1]);
            usage();
            return false;
        }
    }

    if (request.draw_log_frame != 0 && request.frame_seam_addresses.empty()) {
        std::fprintf(stderr,
                     "gmse01_boot: --draw-log-frame names a frame, and there are no frames without "
                     "--render-frames\n");
        return false;
    }
    if (request.draw_skip != 0 && request.frame_seam_addresses.empty()) {
        std::fprintf(stderr,
                     "gmse01_boot: --draw-skip withholds each frame's leading draws, and there are "
                     "no frames without --render-frames\n");
        return false;
    }
    if (request.draw_limit != 0 && request.frame_seam_addresses.empty()) {
        std::fprintf(stderr,
                     "gmse01_boot: --draw-limit bounds each frame's draws, and there are no frames "
                     "without --render-frames\n");
        return false;
    }
    if (request.draw_mode != DrawDiagnosticMode::Normal && request.frame_seam_addresses.empty()) {
        std::fprintf(stderr,
                     "gmse01_boot: --draw-mode %s changes what is drawn, and nothing renders "
                     "without --render-frames\n",
                     draw_diagnostic_mode_name(request.draw_mode));
        return false;
    }
    if (request.frame_image_index != 0 && request.frame_image_path.empty()) {
        std::fprintf(stderr,
                     "gmse01_boot: --dump-frame-index chooses which frame --dump-frame writes, and "
                     "no path was given\n");
        return false;
    }
    if (!request.frame_image_path.empty() && request.frame_seam_addresses.empty()) {
        std::fprintf(stderr,
                     "gmse01_boot: --dump-frame names where to write a rendered frame, but nothing "
                     "renders without --render-frames\n");
        return false;
    }
    if ((request.pad_read_address != 0) != !request.pad_script.empty()) {
        std::fprintf(stderr,
                     "gmse01_boot: --press-buttons and --pad-script are one request: an address "
                     "with no script presses nothing, and a script with no address is a run that "
                     "believes it pressed something\n");
        return false;
    }
    if (request.pad_read_address != 0 && request.frame_seam_addresses.empty()) {
        std::fprintf(stderr,
                     "gmse01_boot: --pad-script is timed in frames, and there are no frames "
                     "without --render-frames\n");
        return false;
    }
    return true;
}

} // namespace sunbright::gcnport_boot
