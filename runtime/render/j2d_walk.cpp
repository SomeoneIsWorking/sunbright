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
#include <cstdarg>
#include <cstdio>

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

}  // namespace

// Published by the J2DScreen::draw tee (runtime/overrides/scene_render.cpp) each
// frame: the live root J2DScreen*. Kept as the single canonical 2D-draw tee.
void sb_j2d_set_root(u32 root) { g_root = root; g_draws++; }

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

// Collect visible J2DPicture quads (screen rect + texture) in draw order.
int sb_j2d_collect(J2dQuad* out, int max, int* screen_w, int* screen_h) {
    const u32 root = g_root;
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
        if (vis && is_picture(kind)) {
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
                if (q.w > 0 && q.h > 0 && valid(q.data)) n++;
            }
        }
        // children, pushed reversed for draw order
        u32 kids[64]; int nk = 0;
        for (u32 link = r32(p + PANE_TREEHEAD); valid(link) && nk < 64; link = r32(link + LINK_NEXT))
            kids[nk++] = r32(link + LINK_DATA);
        for (int i = nk - 1; i >= 0 && sp < 256; i--) stack[sp++] = {kids[i]};
    }
    return n;
}
