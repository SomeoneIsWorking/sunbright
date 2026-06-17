// j2d_walk — N3 of the native-renderer plan (docs/native_port_plan.md §3): read
// the game's J2D (2D HUD/UI) draw data so we can render it natively.
//
// The J2D scene is a tree of J2DPane objects (J2DScreen root → panes → pictures/
// textboxes/windows). The recompiled game builds and updates this tree every
// frame; here we OBSERVE it: a tee on J2DScreen::draw captures the live root, and
// a safe, bounds-checked walk of the pane tree in guest memory extracts each
// pane's kind / bounds / alpha / visibility (+ texture refs for pictures). This
// is the input the native J2D renderer (next step) will consume instead of the
// game's GX draw path. It is read-only and verifiable by inspection now (the HUD
// panes should appear with sane bounds), before any rendering is wired.
//
// Layout (verified against reference/sms JSystem decomp):
//   J2DPane: mKind@0x8 (fourcc), mVisible@0xC, mBounds(JUTRect x1,y1,x2,y2)@0x14,
//            mGlobalMtx@0x84, mAlpha@0xCC, mColorAlpha@0xCD, mPaneTree@0xD0.
//   JSUTree<J2DPane> @0xD0 = JSUList{mHead@0xD0,mTail@0xD4,mLinkCount@0xD8} +
//            JSUPtrLink{mData@0xDC,mPtrList@0xE0,mPrev@0xE4,mNext@0xE8}.
//     → first-child LINK ptr = *(pane+0xD0); from a link: child pane = *(link+0),
//       next sibling link = *(link+0xC).
//   J2DPicture (kind 'PIC1'/'PIC2'): mTextures[4]@0xEC, mTextureNum@0xFC.

#include "../intrinsics.h"     // mem_r32, u32/u8
#include "j2d_types.h"
#include "tex_decode.h"        // sb_tex_decode + pad/size helpers (/texat probe)
#include <cstdint>
#include <vector>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

volatile u32 g_root = 0;        // latest live J2DScreen* seen at draw time
volatile unsigned g_draws = 0;  // how many times the tee published a root

inline bool valid(u32 a) { return a >= 0x80000000u && a < 0x81800000u && (a & 3) == 0; }
inline u32  r32(u32 a) { return valid(a) ? mem_r32(a) : 0; }
inline u8   rb(u32 a) { return valid(a) ? (u8)(mem_r32(a) >> 24) : 0; }  // 4-aligned byte (big-endian)

constexpr u32 PANE_KIND     = 0x08;
constexpr u32 PANE_VISIBLE  = 0x0C;
constexpr u32 PANE_BOUNDS   = 0x14;   // mBounds (local) x1,y1,x2,y2
constexpr u32 PANE_GBOUNDS  = 0x24;   // mGlobalBounds (screen-space) x1,y1,x2,y2
constexpr u32 PANE_ALPHA    = 0xCC;
constexpr u32 PANE_TREEHEAD = 0xD0;   // JSUList::mHead (first child LINK)
constexpr u32 LINK_DATA     = 0x00;   // JSUPtrLink::mData  (owner pane)
constexpr u32 LINK_NEXT     = 0x0C;   // JSUPtrLink::mNext  (next sibling link)
constexpr u32 PIC_TEXTURES  = 0xEC;
constexpr u32 PIC_TEXNUM    = 0xFC;
constexpr u32 PIC_WHITE     = 0x13C;  // J2DPicture mWhite (TColor 0xRRGGBBAA)
constexpr u32 PIC_BLACK     = 0x140;  // J2DPicture mBlack
constexpr u32 PIC_CORNERCOL = 0x144;  // mCornerColor[4] (header's 0x114 comment is a typo; real
                                      // offset is after mBlack@0x140 → mBlendKonstColor@0x154 ✓)
// JUTTexture fields
constexpr u32 TEX_DATA      = 0x24;   // mTexData (raw tiled image bytes)
constexpr u32 TEX_EMBPAL    = 0x28;   // mEmbPalette (JUTPalette*)
constexpr u32 TEX_FORMAT    = 0x34;   // mFormat (GX TextureFormat)
constexpr u32 TEX_WH        = 0x3C;   // mWidth(u16)@0x3C, mHeight(u16)@0x3E

inline bool is_picture(u32 kind) {    // 'PIC1'/'PIC2'
    return (kind >> 8) == 0x504943u;  // "PIC"
}
inline bool is_textbox(u32 kind) { return kind == 0x54425831u; }   // 'TBX1'
inline bool is_window(u32 kind)  { return kind == 0x57494E31u; }   // 'WIN1'

// J2DWindow contents colors (reference/sms J2DWindow::drawContents quad order):
// TL=unk118, TR=unk120, BR=unk124, BL=unk11C; mColorAlpha@0xCD.
constexpr u32 WIN_COL_TL = 0x118;
constexpr u32 WIN_COL_BL = 0x11C;
constexpr u32 WIN_COL_TR = 0x120;
constexpr u32 WIN_COL_BR = 0x124;
constexpr u32 PANE_COLORALPHA = 0xCD;

inline u16 r16(u32 a) { const u32 w = r32(a & ~3u); return (a & 2) ? (u16)(w & 0xFFFF) : (u16)(w >> 16); }
inline u8  rb8(u32 a) { return valid(a & ~3u) ? (u8)(mem_r32(a & ~3u) >> (24 - (a & 3) * 8)) : 0; }  // any-align byte

// J2DTextBox fields (reference/sms J2DTextBox.hpp)
constexpr u32 TBX_FONT      = 0xEC;   // JUTFont* (JUTResFont for SMS HUD)
constexpr u32 TBX_CHARCOLOR = 0xF0;   // TColor 0xRRGGBBAA
constexpr u32 TBX_GRADCOLOR = 0xF4;
constexpr u32 TBX_HBIND     = 0xF8;   // 0=center,1=right,2=left
constexpr u32 TBX_VBIND     = 0xFC;   // 0=center,1=bottom,2=top
constexpr u32 TBX_TEXT      = 0x100;  // char*
constexpr u32 TBX_CHARSPACE = 0x10C;
constexpr u32 TBX_LINESPACE = 0x110;
constexpr u32 TBX_FONTSZX   = 0x114;
constexpr u32 TBX_FONTSZY   = 0x118;
// JUTResFont fields (reference/sms JUTResFont.hpp)
constexpr u32 RF_INFO   = 0x4C;  // ResFONT::INF1*
constexpr u32 RF_WIDS   = 0x50;  // WID1**
constexpr u32 RF_GLYS   = 0x54;  // GLY1**
constexpr u32 RF_MAPS   = 0x58;  // MAP1**
constexpr u32 RF_NWID   = 0x5C;  // u16
constexpr u32 RF_NGLY   = 0x5E;
constexpr u32 RF_NMAP   = 0x60;

}  // namespace

// Ring of the most-recent distinct J2DScreen roots seen at draw time. Used by the
// /j2dscreens correlation probe to answer "are there MULTIPLE screens drawn per frame,
// and which one holds the (visible) menu windows?" — the single-snapshot capture only
// keeps the LAST root, so if the menu lives in an earlier screen ngx never renders it.
constexpr int J2D_ROOT_RING = 16;
static u32      g_root_ring[J2D_ROOT_RING] = {0};
static unsigned g_root_seq[J2D_ROOT_RING]  = {0};
static std::atomic<int> g_root_rp{0};

// Published by the J2DScreen::draw tee (runtime/overrides/scene_render.cpp) each
// frame: the live root J2DScreen*. Kept as the single canonical 2D-draw tee.
void sb_j2d_set_root(u32 root) {
    g_root = root; g_draws++;
    const int i = g_root_rp.load(std::memory_order_relaxed) % J2D_ROOT_RING;
    g_root_ring[i] = root; g_root_seq[i] = g_draws;
    g_root_rp.store((i + 1), std::memory_order_relaxed);
}

// Dump the captured pane tree. Returns the number of panes visited.
int sb_j2d_dump(char* out, int cap) {
    int pos = 0;
    auto app = [&](const char* fmt, ...) {
        if (pos >= cap) return;
        va_list ap; va_start(ap, fmt);
        pos += vsnprintf(out + pos, cap - pos, fmt, ap);
        va_end(ap);
    };

    const u32 root = g_root;
    app("j2d: draws=%u root=%08x\n", g_draws, root);
    if (!valid(root)) { app("(no valid root yet — is a J2D screen drawing? fastboot+gameplay)\n"); return 0; }

    // Iterative DFS over (pane, depth). The child/sibling links keep us honest;
    // every dereference is bounds-checked and counts are capped so a corrupt/
    // mid-update pointer can't wild-read or loop forever.
    struct Item { u32 pane; int depth; };
    Item stack[256];
    int sp = 0, visited = 0;
    stack[sp++] = {root, 0};

    while (sp > 0 && visited < 1024) {
        Item it = stack[--sp];
        const u32 p = it.pane;
        if (!valid(p)) continue;
        visited++;

        const u32 kind = r32(p + PANE_KIND);
        char k[5] = {(char)(kind >> 24), (char)(kind >> 16), (char)(kind >> 8), (char)kind, 0};
        for (int i = 0; i < 4; i++) if (k[i] < 32 || k[i] > 126) k[i] = '.';
        const int x1 = (int)r32(p + PANE_BOUNDS + 0), y1 = (int)r32(p + PANE_BOUNDS + 4);
        const int x2 = (int)r32(p + PANE_BOUNDS + 8), y2 = (int)r32(p + PANE_BOUNDS + 12);

        app("%*s%08x '%s' [%d,%d %dx%d] a=%u vis=%u", it.depth * 2, "", p, k,
            x1, y1, x2 - x1, y2 - y1, rb(p + PANE_ALPHA), rb(p + PANE_VISIBLE));
        if (k[0] == 'P' && k[1] == 'I' && k[2] == 'C') {   // J2DPicture
            const u8 ntex = rb(p + PIC_TEXNUM);
            app(" tex=%u t0=%08x", ntex, r32(p + PIC_TEXTURES));
        }
        app("\n");

        // Push children (collect first, then push reversed so they print in order).
        u32 kids[64]; int nk = 0;
        for (u32 link = r32(p + PANE_TREEHEAD); valid(link) && nk < 64; link = r32(link + LINK_NEXT))
            kids[nk++] = r32(link + LINK_DATA);
        for (int i = nk - 1; i >= 0 && sp < 256; i--)
            stack[sp++] = {kids[i], it.depth + 1};
    }
    app("j2d: %d panes visited%s\n", visited, visited >= 1024 ? " (capped)" : "");
    return visited;
}

static bool win_tex(u32 texptr, u32& data, int& fmt, int& w, int& h);   // defined below

// Walk one root and tally pane kinds + every WINDOW (total / visible) with its rect and
// fill colours. A window is "visible" only if it AND every ancestor on its path are
// vis!=0 (J2DPane::draw prunes an invisible subtree), matching what actually renders.
static void tally_root(u32 root, char* out, int cap, int* pos) {
    auto app = [&](const char* fmt, ...) {
        if (*pos >= cap) return; va_list ap; va_start(ap, fmt);
        *pos += vsnprintf(out + *pos, cap - *pos, fmt, ap); va_end(ap);
    };
    if (!valid(root)) { app("  (invalid root)\n"); return; }
    struct Item { u32 pane; int vis_chain; };
    Item stack[256]; int sp = 0, seen = 0;
    int npic = 0, ntbx = 0, nwin = 0, nwin_vis = 0;
    stack[sp++] = {root, 1};
    while (sp > 0 && seen < 1024) {
        Item it = stack[--sp];
        const u32 p = it.pane; if (!valid(p)) continue; seen++;
        const u32 kind = r32(p + PANE_KIND);
        const int vis_here = it.vis_chain && (rb(p + PANE_VISIBLE) != 0);
        if (is_picture(kind)) npic++;
        else if (is_textbox(kind)) ntbx++;
        else if (is_window(kind)) {
            nwin++;
            int gx0 = (int)r32(p + PANE_GBOUNDS + 0), gy0 = (int)r32(p + PANE_GBOUNDS + 4);
            int gx1 = (int)r32(p + PANE_GBOUNDS + 8), gy1 = (int)r32(p + PANE_GBOUNDS + 12);
            if (vis_here) nwin_vis++;
            app("    WIN1 %08x vis=%d(self=%u) [%d,%d %dx%d] a=%u ca=%u TL=%08x TR=%08x BL=%08x BR=%08x\n",
                p, vis_here, rb(p + PANE_VISIBLE), gx0, gy0, gx1 - gx0, gy1 - gy0,
                rb(p + PANE_ALPHA), rb8(p + PANE_COLORALPHA),
                r32(p + WIN_COL_TL), r32(p + WIN_COL_TR), r32(p + WIN_COL_BL), r32(p + WIN_COL_BR));
            // Border 9-slice texture objects + the white/black remap regs (unk128/unk12C).
            const char* nm[4] = {"t100", "t104", "t108", "t10C"};
            for (int b = 0; b < 4; b++) {
                const u32 tp = r32(p + 0x100 + (u32)b * 4);
                u32 dd; int ff, ww, hh;
                if (win_tex(tp, dd, ff, ww, hh))
                    app("      %s=%08x data=%08x fmt=%d %dx%d\n", nm[b], tp, dd, ff, ww, hh);
                else
                    app("      %s=%08x (no/invalid tex)\n", nm[b], tp);
            }
            app("      white(0x128)=%08x black(0x12C)=%08x fl(0x114)=%08x tex110(0x110)=%08x\n",
                r32(p + 0x128), r32(p + 0x12C), r32(p + 0x114), r32(p + 0x110));
        }
        u32 kids[64]; int nk = 0;
        for (u32 link = r32(p + PANE_TREEHEAD); valid(link) && nk < 64; link = r32(link + LINK_NEXT))
            kids[nk++] = r32(link + LINK_DATA);
        for (int i = nk - 1; i >= 0 && sp < 256; i--) stack[sp++] = {kids[i], vis_here};
    }
    app("    => panes=%d pic=%d tbx=%d win=%d win_visible=%d\n", seen, npic, ntbx, nwin, nwin_vis);
}

// /texat?a=HEX&fmt=N&w=N&h=N — decode a guest texture exactly as ngx's texture_for does
// (same block-padding) and report an intensity/alpha grid so we can SEE what the renderer
// samples (e.g. the white window-border IA4 texture). Pure read; Dolphin-free.
int sb_tex_at_dump(char* out, int cap, u32 addr, int fmt, int w, int h) {
    int pos = 0;
    auto app = [&](const char* fmt2, ...) {
        if (pos >= cap) return; va_list ap; va_start(ap, fmt2);
        pos += vsnprintf(out + pos, cap - pos, fmt2, ap); va_end(ap);
    };
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) { app("texat: bad w/h\n"); return 0; }
    const int pw = sb_tex_pad_w(w, fmt), ph = sb_tex_pad_h(h, fmt);
    const int srcbytes = sb_tex_size_bytes(pw, ph, fmt);
    const u8* host = (addr >= 0x80000000u) ? (const u8*)sb_ram_fast(addr) : nullptr;
    app("texat a=%08x fmt=%d w=%d h=%d pad=%dx%d srcbytes=%d host=%p\n",
        addr, fmt, w, h, pw, ph, srcbytes, (const void*)host);
    if (!host || srcbytes <= 0) { app("texat: no host or bad size\n"); return 0; }
    static std::vector<uint32_t> dec; dec.assign((size_t)pw * ph, 0);
    sb_tex_decode(dec.data(), host, pw, ph, fmt, nullptr, 0);
    // overall stats on the logical w×h region
    int rmin=255,rmax=0,amin=255,amax=0; long rsum=0,asum=0,nn=0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const uint32_t v = dec[(size_t)y * pw + x];
        const int r = v & 0xFF, a = (v >> 24) & 0xFF;
        if (r<rmin)rmin=r; if(r>rmax)rmax=r; if(a<amin)amin=a; if(a>amax)amax=a; rsum+=r; asum+=a; nn++;
    }
    app("  R(intensity) min=%d max=%d mean=%ld | A min=%d max=%d mean=%ld\n",
        rmin, rmax, (nn?rsum/nn:0), amin, amax, (nn?asum/nn:0));
    // ASCII intensity grid (R channel) downsampled to <=16 cols
    const int gc = w < 16 ? w : 16, gr = h < 16 ? h : 16;
    const char* ramp = " .:-=+*#%@";
    for (int gy = 0; gy < gr; gy++) {
        char line[40]; int lp = 0;
        for (int gx = 0; gx < gc; gx++) {
            const int sx = gx * w / gc, sy = gy * h / gr;
            const uint32_t v = dec[(size_t)sy * pw + sx];
            const int r = v & 0xFF;
            line[lp++] = ramp[r * 9 / 255];
        }
        line[lp] = 0; app("  |%s|\n", line);
    }
    return 1;
}

// /j2dscreens — dump every recently-seen J2DScreen root with its window inventory, so we
// can tell whether the menu windows live in a screen OTHER than the last-captured g_root.
int sb_j2d_screens_dump(char* out, int cap) {
    int pos = 0;
    auto app = [&](const char* fmt, ...) {
        if (pos >= cap) return; va_list ap; va_start(ap, fmt);
        pos += vsnprintf(out + pos, cap - pos, fmt, ap); va_end(ap);
    };
    {   // screen dims the present uses to map J2D px → framebuffer NDC (snapshot sw/sh).
        int sw = 0, sh = 0;
        if (valid(g_root)) {
            sw = (int)r32(g_root + PANE_GBOUNDS + 8) - (int)r32(g_root + PANE_GBOUNDS + 0);
            sh = (int)r32(g_root + PANE_GBOUNDS + 12) - (int)r32(g_root + PANE_GBOUNDS + 4);
        }
        app("j2dscreens: draws=%u last_root=%08x screen=%dx%d\n", g_draws, g_root, sw, sh);
    }
    // Print distinct roots in the ring, most-recent first, dedup by pointer.
    u32 seen_roots[J2D_ROOT_RING]; int ns = 0;
    const int rp = g_root_rp.load(std::memory_order_relaxed);
    for (int k = 1; k <= J2D_ROOT_RING; k++) {
        const int i = ((rp - k) % J2D_ROOT_RING + J2D_ROOT_RING) % J2D_ROOT_RING;
        const u32 r = g_root_ring[i]; if (!valid(r)) continue;
        bool dup = false; for (int j = 0; j < ns; j++) if (seen_roots[j] == r) { dup = true; break; }
        if (dup) continue; seen_roots[ns++] = r;
        app("  root=%08x last_seq=%u\n", r, g_root_seq[i]);
        tally_root(r, out, cap, &pos);
    }
    app("j2dscreens: %d distinct roots in last %d draws\n", ns, J2D_ROOT_RING);
    return ns;
}

// ── J2DTextBox font rendering (port of JUTResFont getFontCode/loadImage/drawChar) ──
// chr → glyph index via the MAP1 blocks (reference/sms JUTResFont::getFontCode).
static int font_code(u32 font, int chr) {
    const u32 info = r32(font + RF_INFO);
    const int def  = valid(info) ? r16(info + 0x12) : chr;   // INF1.defaultCode
    const u32 maps = r32(font + RF_MAPS);
    const int nmap = r16(font + RF_NMAP);
    if (!valid(maps)) return chr;                            // no map → assume direct
    for (int i = 0; i < nmap && i < 16; i++) {
        const u32 m = r32(maps + i * 4);
        if (!valid(m)) continue;
        const int method = r16(m + 0x08), start = r16(m + 0x0A), end = r16(m + 0x0C);
        if (chr < start || chr > end) continue;
        if (method == 0) return chr - start;                          // direct
        if (method == 2) return r16(m + 0x10 + (u32)(chr - start) * 2); // index table
        if (method == 3) {                                            // sorted (code,index) pairs
            const u32 base = m + 0x10; int lo = 0, hi = (int)r16(m + 0x0E) - 1;
            while (hi >= lo) { const int mid = (lo + hi) / 2, c = r16(base + (u32)mid * 4);
                if (chr < c) hi = mid - 1; else if (chr > c) lo = mid + 1; else return r16(base + (u32)mid * 4 + 2); }
        }
        return def;   // method 1 (SJIS) unsupported here
    }
    return def;
}

struct GlyphCell { u32 page; int fmt, texW, texH; int px, py; bool ok; };
// glyph index → atlas page + cell pixel origin (reference/sms JUTResFont::loadImage).
static GlyphCell load_glyph(u32 font, int code) {
    GlyphCell g{};
    const u32 glys = r32(font + RF_GLYS); const int ngly = r16(font + RF_NGLY);
    if (!valid(glys)) return g;
    for (int i = 0; i < ngly && i < 16; i++) {
        const u32 gb = r32(glys + i * 4);
        if (!valid(gb)) continue;
        const int start = r16(gb + 0x08), end = r16(gb + 0x0A);
        if (code < start || code > end) continue;
        const int c = code - start;
        const int cellW = r16(gb + 0x0C), cellH = r16(gb + 0x0E);
        const u32 texSize = r32(gb + 0x10);
        const int numRows = r16(gb + 0x16), numCols = r16(gb + 0x18);
        const int pageCells = numRows * numCols;
        if (pageCells <= 0) return g;
        const int pageIdx = c / pageCells, inPage = c % pageCells;
        const int row = inPage / numRows, col = inPage - row * numRows;   // decomp uses numRows for both
        g.px = col * cellW; g.py = row * cellH;
        g.fmt = r16(gb + 0x14); g.texW = r16(gb + 0x1A); g.texH = r16(gb + 0x1C);
        g.page = gb + 0x20 + (u32)pageIdx * texSize; g.ok = true;
        return g;
    }
    return g;
}
// glyph advance width (reference/sms JUTResFont::getWidthEntry; TWidth{kern@0, width@1}).
static int glyph_width(u32 font, int code, int def_w) {
    const u32 wids = r32(font + RF_WIDS); const int nwid = r16(font + RF_NWID);
    if (!valid(wids)) return def_w;
    for (int i = 0; i < nwid && i < 16; i++) {
        const u32 wb = r32(wids + i * 4);
        if (!valid(wb)) continue;
        const int start = r16(wb + 0x08), end = r16(wb + 0x0A);
        if (code < start || code > end) continue;
        return rb8(wb + 0x0C + (u32)(code - start) * 2 + 1);   // field_0x1 = advance
    }
    return def_w;
}

// Emit one textured quad per glyph of a J2DTextBox's string (atlas cell → UV sub-rect).
static int emit_textbox(u32 p, J2dQuad* out, int max, int n) {
    const u32 font = r32(p + TBX_FONT), text = r32(p + TBX_TEXT);
    if (!valid(font) || !valid(text)) return n;
    const u32 info = r32(font + RF_INFO);
    if (!valid(info)) return n;
    const int ascent = r16(info + 0x0A), descent = r16(info + 0x0C), fwidth = r16(info + 0x0E);
    const int height = ascent + descent;
    if (height <= 0 || fwidth <= 0) return n;
    int szx = (int)r32(p + TBX_FONTSZX), szy = (int)r32(p + TBX_FONTSZY);
    if (szx <= 0) szx = fwidth;
    if (szy <= 0) szy = height;
    const int charSpace = (int)r32(p + TBX_CHARSPACE), lineSpace = (int)r32(p + TBX_LINESPACE);
    u32 charCol = r32(p + TBX_CHARCOLOR);
    if (charCol == 0) charCol = 0xFFFFFFFFu;   // uninit → white (else invisible)
    const u8 alpha = rb(p + PANE_ALPHA);
    int gx0 = (int)r32(p + PANE_GBOUNDS + 0), gy0 = (int)r32(p + PANE_GBOUNDS + 4);
    const int gx1 = (int)r32(p + PANE_GBOUNDS + 8), gy1 = (int)r32(p + PANE_GBOUNDS + 12);
    if (gx1 <= gx0 || gy1 <= gy0) { gx0 = (int)r32(p + PANE_BOUNDS + 0); gy0 = (int)r32(p + PANE_BOUNDS + 4); }
    const float sxScale = (float)szx, syScale = (float)szy;
    float penx = (float)gx0;
    float baseY = (float)gy0 + (float)ascent * (syScale / height);   // baseline
    for (int k = 0; k < 512 && n < max; k++) {
        const int ch = rb8(text + (u32)k);
        if (ch == 0) break;
        if (ch == '\n') { penx = (float)gx0; baseY += syScale + lineSpace; continue; }
        const int code = font_code(font, ch);
        const int adv = glyph_width(font, code, fwidth);
        const GlyphCell g = load_glyph(font, code);
        if (ch != ' ' && g.ok && g.texW > 0 && g.texH > 0 && valid(g.page)) {
            J2dQuad& q = out[n];
            q.x0 = (int)penx; q.x1 = (int)(penx + sxScale);
            q.y0 = (int)(baseY - (float)ascent * (syScale / height));
            q.y1 = (int)(baseY + (float)descent * (syScale / height));
            q.alpha = alpha; q.fmt = g.fmt; q.w = g.texW; q.h = g.texH; q.data = g.page;
            q.tlut = 0; q.tlutfmt = 0;
            for (int c = 0; c < 4; c++) q.corner[c] = charCol;
            q.white = 0xFFFFFFFFu; q.black = 0;   // atlas intensity × charColor
            q.u0 = (float)g.px / g.texW;          q.v0 = (float)g.py / g.texH;
            q.u1 = (float)(g.px + fwidth) / g.texW; q.v1 = (float)(g.py + height) / g.texH;
            n++;
        }
        penx += (float)adv * (sxScale / fwidth) + charSpace;
    }
    return n;
}

// Read a J2DWindow::Texture* (a JUTTexture) → quad texture description.
static bool win_tex(u32 texptr, u32& data, int& fmt, int& w, int& h) {
    if (!valid(texptr)) return false;
    data = r32(texptr + TEX_DATA);
    fmt  = (int)(r32(texptr + TEX_FORMAT) & 0xFF);
    const u32 wh = r32(texptr + TEX_WH);
    w = (int)(wh >> 16); h = (int)(wh & 0xFFFF);
    return valid(data) && w > 0 && h > 0;
}

// J2DWindow: contents fill + textured 9-slice border (port of J2DWindow::draw_private/
// drawContents, reference/sms). The 4 corner textures (unk100=TL/104=TR/108=BL/10C=BR)
// are placed at the corners and STRETCHED along the edges between them; UV flips come
// from the unk114 flag bits (encoded here as swapped uvrect endpoints, matching
// Texture::draw's 0x8000=1.0 / 0=0.0 texcoords).
static int emit_window(u32 p, J2dQuad* out, int max, int n) {
    int gx0 = (int)r32(p + PANE_GBOUNDS + 0), gy0 = (int)r32(p + PANE_GBOUNDS + 4);
    int gx1 = (int)r32(p + PANE_GBOUNDS + 8), gy1 = (int)r32(p + PANE_GBOUNDS + 12);
    if (gx1 <= gx0 || gy1 <= gy0) {
        gx0 = (int)r32(p + PANE_BOUNDS + 0); gy0 = (int)r32(p + PANE_BOUNDS + 4);
        gx1 = (int)r32(p + PANE_BOUNDS + 8); gy1 = (int)r32(p + PANE_BOUNDS + 12);
    }
    const int W = gx1 - gx0, H = gy1 - gy0;
    if (W <= 0 || H <= 0) return n;
    const u8 alpha = rb8(p + PANE_COLORALPHA);
    u32 wht = r32(p + 0x128), blk = r32(p + 0x12C);   // window texture white/black remap
    if (wht == 0) wht = 0xFFFFFFFFu;
    const u32 fl = r32(p + 0x114);

    // Contents fill (4-colour quad via the 1×1 white fallback).
    if (n < max) {
        J2dQuad& q = out[n];
        q.x0 = gx0; q.y0 = gy0; q.x1 = gx1; q.y1 = gy1;
        q.alpha = alpha; q.fmt = 0; q.w = 0; q.h = 0; q.data = 0; q.tlut = 0; q.tlutfmt = 0;
        q.corner[0] = r32(p + WIN_COL_TL); q.corner[1] = r32(p + WIN_COL_TR);
        q.corner[2] = r32(p + WIN_COL_BL); q.corner[3] = r32(p + WIN_COL_BR);
        q.white = 0xFFFFFFFFu; q.black = 0;
        q.u0 = 0.f; q.v0 = 0.f; q.u1 = 1.f; q.v1 = 1.f;
        n++;
    }

    // Textured 9-slice border.
    const u32 t100 = r32(p + 0x100), t104 = r32(p + 0x104), t108 = r32(p + 0x108), t10C = r32(p + 0x10C);
    auto add = [&](u32 tex, int sx, int sy, int sw, int sh, float u0, float v0, float u1, float v1) {
        u32 dd; int ff, ww, hh;
        if (n >= max || sw <= 0 || sh <= 0 || !win_tex(tex, dd, ff, ww, hh)) return;
        J2dQuad& q = out[n];
        q.x0 = gx0 + sx; q.y0 = gy0 + sy; q.x1 = gx0 + sx + sw; q.y1 = gy0 + sy + sh;
        q.alpha = alpha; q.fmt = ff; q.w = ww; q.h = hh; q.data = dd; q.tlut = 0; q.tlutfmt = 0;
        for (int c = 0; c < 4; c++) q.corner[c] = 0xFFFFFFFFu;
        q.white = wht; q.black = blk;
        q.u0 = u0; q.v0 = v0; q.u1 = u1; q.v1 = v1;
        n++;
    };
    u32 dd; int ff, w100 = 0, h100 = 0, w104 = 0, h104 = 0, w108 = 0, h108 = 0, w10C = 0, h10C = 0;
    win_tex(t100, dd, ff, w100, h100); win_tex(t104, dd, ff, w104, h104);
    win_tex(t108, dd, ff, w108, h108); win_tex(t10C, dd, ff, w10C, h10C);
    const int iV6 = W - w10C, iV8 = H - h10C;
    auto fU = [](bool h){ return h ? 1.f : 0.f; };          // flipped-left U
    // Corners (hflip/vflip → swap uvrect endpoints).
    add(t100, 0,   0,   w100, h100, fU(fl&0x80), fU(fl&0x40), fU(!(fl&0x80)), fU(!(fl&0x40)));
    add(t104, iV6, 0,   w104, h104, fU(fl&0x20), fU(fl&0x10), fU(!(fl&0x20)), fU(!(fl&0x10)));
    add(t108, 0,   iV8, w108, h108, fU(fl&0x08), fU(fl&0x04), fU(!(fl&0x08)), fU(!(fl&0x04)));
    add(t10C, iV6, iV8, w10C, h10C, fU(fl&0x02), fU(fl&0x01), fU(!(fl&0x02)), fU(!(fl&0x01)));
    // Edges (uvrect derived from J2DWindow::draw_private's explicit texcoords).
    { float a=fU(fl&0x20), c=fU(!(fl&0x10)), d=1.f-c;                                  // TOP (unk104)
      add(t104, w100, 0,   iV6 - w100, h104, a, d, a, c); }
    { float a=fU(fl&0x02), b=fU(!(fl&0x01)), c=1.f-b;                                  // BOTTOM (unk10C)
      add(t10C, w100, iV8, iV6 - w100, h10C, a, c, a, b); }
    { float a=fU(!(fl&0x08)), b=1.f-a, c=fU(fl&0x04);                                  // LEFT (unk108)
      add(t108, 0,   h100, w108, iV8 - h100, b, c, a, c); }
    { float a=fU(!(fl&0x02)), b=1.f-a, c=fU(fl&0x01);                                  // RIGHT (unk10C)
      add(t10C, iV6, h100, w10C, iV8 - h100, b, c, a, c); }
    return n;
}

// Collect visible J2DPicture quads (screen rect + texture) in draw order, from a
// given J2DScreen root. Pure read of guest memory — caller supplies the consistency
// (snapshot at draw time on the game thread, or accept the live-read race for probes).
static int collect_from(u32 root, J2dQuad* out, int max, int* screen_w, int* screen_h) {
    if (screen_w) *screen_w = 0;
    if (screen_h) *screen_h = 0;
    if (!valid(root) || !out || max <= 0) return 0;

    if (screen_w) *screen_w = (int)r32(root + PANE_GBOUNDS + 8) - (int)r32(root + PANE_GBOUNDS + 0);
    if (screen_h) *screen_h = (int)r32(root + PANE_GBOUNDS + 12) - (int)r32(root + PANE_GBOUNDS + 4);
    // Fall back to local bounds if global isn't populated (degenerate size).
    if (screen_w && *screen_w <= 0) *screen_w = (int)r32(root + PANE_BOUNDS + 8) - (int)r32(root + PANE_BOUNDS + 0);
    if (screen_h && *screen_h <= 0) *screen_h = (int)r32(root + PANE_BOUNDS + 12) - (int)r32(root + PANE_BOUNDS + 4);

    struct Item { u32 pane; };
    Item stack[256]; int sp = 0, n = 0, seen = 0;
    stack[sp++] = {root};
    while (sp > 0 && seen < 1024 && n < max) {
        const u32 p = stack[--sp].pane;
        if (!valid(p)) continue;
        seen++;

        const u32 kind = r32(p + PANE_KIND);
        const bool vis = rb(p + PANE_VISIBLE) != 0;
        // J2DPane::draw early-returns on an invisible pane: it draws neither itself
        // NOR its children. So an invisible pane prunes its whole subtree — e.g. the
        // counter-roll widget keeps its off-screen digit rows as vis=0 PAN1 parents
        // whose PIC1 children are vis=1; descending into them would draw the hidden
        // rows (the garbled HUD overlap). Skip the subtree entirely.
        if (!vis) continue;
        if (is_picture(kind)) {
            const u32 tex = r32(p + PIC_TEXTURES);   // mTextures[0]
            if (valid(tex)) {
                // Prefer global bounds; fall back to local if empty.
                int x0 = (int)r32(p + PANE_GBOUNDS + 0), y0 = (int)r32(p + PANE_GBOUNDS + 4);
                int x1 = (int)r32(p + PANE_GBOUNDS + 8), y1 = (int)r32(p + PANE_GBOUNDS + 12);
                if (x1 <= x0 || y1 <= y0) {
                    x0 = (int)r32(p + PANE_BOUNDS + 0); y0 = (int)r32(p + PANE_BOUNDS + 4);
                    x1 = (int)r32(p + PANE_BOUNDS + 8); y1 = (int)r32(p + PANE_BOUNDS + 12);
                }
                const u32 wh = r32(tex + TEX_WH);
                J2dQuad& q = out[n];
                q.x0 = x0; q.y0 = y0; q.x1 = x1; q.y1 = y1;
                q.alpha = rb(p + PANE_ALPHA);
                q.fmt = (int)(r32(tex + TEX_FORMAT) & 0xFF);
                q.w = (int)(wh >> 16); q.h = (int)(wh & 0xFFFF);
                q.data = r32(tex + TEX_DATA);
                q.tlut = 0; q.tlutfmt = 0;   // palette resolution: later (M+)
                for (int c = 0; c < 4; c++) q.corner[c] = r32(p + PIC_CORNERCOL + c * 4);
                q.white = r32(p + PIC_WHITE); q.black = r32(p + PIC_BLACK);
                q.u0 = 0.f; q.v0 = 0.f; q.u1 = 1.f; q.v1 = 1.f;   // whole texture
                if (q.w > 0 && q.h > 0 && valid(q.data)) n++;
            }
        } else if (is_textbox(kind)) {
            n = emit_textbox(p, out, max, n);   // one quad per glyph (font atlas cells)
        } else if (is_window(kind)) {
            n = emit_window(p, out, max, n);   // contents fill + textured 9-slice border
        }
        // children, pushed reversed for draw order
        u32 kids[64]; int nk = 0;
        for (u32 link = r32(p + PANE_TREEHEAD); valid(link) && nk < 64; link = r32(link + LINK_NEXT))
            kids[nk++] = r32(link + LINK_DATA);
        for (int i = nk - 1; i >= 0 && sp < 256; i--) stack[sp++] = {kids[i]};
    }
    return n;
}

// Live walk of the current root — used by the /j2drender + /j2d diagnostic probes
// (HTTP thread). Accepts the read-vs-update race; for the production present path use
// the draw-time snapshot below.
int sb_j2d_collect(J2dQuad* out, int max, int* screen_w, int* screen_h) {
    return collect_from(g_root, out, max, screen_w, screen_h);
}

// ── Draw-time snapshot ─────────────────────────────────────────────────────────
// The native present (video thread) needs a CONSISTENT HUD draw list. J2DPane::draw
// computes mGlobalBounds/mColorAlpha into each pane DURING the draw; reading the tree
// live from another thread catches it half-updated (digits land at stale x → the HUD
// smears). So we capture on the game thread right after J2DScreen::draw returns (the
// tree is fully, consistently positioned) into a double buffer the video thread reads.
constexpr int J2D_SNAP_MAX = 1024;   // pictures (~105) + textbox glyphs
static J2dQuad g_snap[2][J2D_SNAP_MAX];
static int g_snap_n[2] = {0, 0};
static int g_snap_w[2] = {0, 0}, g_snap_h[2] = {0, 0};
static std::atomic<int> g_snap_front{0};

// Called from the J2DScreen::draw tee (game thread) AFTER the real draw, when bounds
// are freshly and consistently computed.
void sb_j2d_capture(u32 root) {
    const int back = 1 - g_snap_front.load(std::memory_order_relaxed);
    g_snap_n[back] = collect_from(root, g_snap[back], J2D_SNAP_MAX, &g_snap_w[back], &g_snap_h[back]);
    g_snap_front.store(back, std::memory_order_release);
}

// Read the latest consistent HUD draw list (video thread). Returns the quad count.
int sb_j2d_snapshot(J2dQuad* out, int max, int* screen_w, int* screen_h) {
    const int f = g_snap_front.load(std::memory_order_acquire);
    int n = g_snap_n[f]; if (n > max) n = max;
    if (n > 0) std::memcpy(out, g_snap[f], (size_t)n * sizeof(J2dQuad));
    if (screen_w) *screen_w = g_snap_w[f];
    if (screen_h) *screen_h = g_snap_h[f];
    return n;
}
