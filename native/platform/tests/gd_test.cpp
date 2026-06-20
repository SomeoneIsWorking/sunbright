// GD seam test — the GameCube GD (GX Display-list builder) library, compiled from
// the real decomp source (reference/sms/src/dolphin/gd). GD bakes big-endian GX
// command bytes into a memory buffer; the J3D loader uses it to build each shape's
// VCD/VAT command buffer at load time. This asserts SPEC-COMPUTED DL bytes for a
// known vertex-descriptor list — the tested function IS the shipping GDSetVtxDescv.
//
// Spec derivation (GDGeometry.c + GDBase.h + GDGeometry.h), list =
//   POS=INDEX16(3), NRM=INDEX16(3), CLR0=INDEX16(3), TEX0=INDEX16(3):
//   defaults pnMtxIdx=NONE(0), txMtxIdxMask=0, posn->3, norm->3 (nnorms=1),
//            col0->3 (ncols=1), col1=NONE(0), tex0->3 (ntexs=1).
//   CP_REG_VCD_LO = posn<<9 | norm<<11 | col0<<13 = 0x600|0x1800|0x6000 = 0x7E00
//   CP_REG_VCD_HI = tex0<<0 = 0x3
//   XF_REG_INVTXSPEC = ncols<<0 | nnorms<<2 | ntexs<<4 = 1|4|0x10 = 0x15
// Byte stream (GDWriteCPCmd = 0x08,id,u32BE ; GDWriteXFCmd = 0x10,u16BE(0),u16BE(addr),u32BE):
//   CP 0x50 0x00007E00 -> 08 50 00 00 7E 00
//   CP 0x60 0x00000003 -> 08 60 00 00 00 03
//   XF 0x1008 0x15      -> 10 00 00 10 08 00 00 00 15
#include <dolphin/gd/GDBase.h>
#include <dolphin/gd/GDGeometry.h>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXStruct.h>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main() {
    // GC DL buffers are always 32-byte aligned; GDPadCurr32 aligns the ABSOLUTE
    // pointer, so the test buffer must be 32-aligned for offset==absolute.
    alignas(32) unsigned char buf[256];
    std::memset(buf, 0xAA, sizeof(buf));

    GDLObj dl;
    GDInitGDLObj(&dl, buf, sizeof(buf));
    __GDCurrentDL = &dl;

    GXVtxDescList list[] = {
        { GX_VA_POS,  GX_INDEX16 },
        { GX_VA_NRM,  GX_INDEX16 },
        { GX_VA_CLR0, GX_INDEX16 },
        { GX_VA_TEX0, GX_INDEX16 },
        { (GXAttr)0xff, GX_NONE },
    };
    GDSetVtxDescv(list);

    const unsigned char expect[] = {
        0x08, 0x50, 0x00, 0x00, 0x7E, 0x00,             // CP VCD_LO = 0x7E00
        0x08, 0x60, 0x00, 0x00, 0x00, 0x03,             // CP VCD_HI = 0x03
        0x10, 0x00, 0x00, 0x10, 0x08, 0x00, 0x00, 0x00, 0x15, // XF INVTXSPEC = 0x15
    };
    const int n = (int)sizeof(expect);

    // The DL pointer advanced exactly n bytes.
    CHECK(dl.ptr - dl.start == n);
    // The emitted bytes match the spec byte-for-byte.
    CHECK(std::memcmp(buf, expect, n) == 0);
    if (std::memcmp(buf, expect, n) != 0) {
        printf("got:   ");
        for (int i = 0; i < n; ++i) printf("%02X ", buf[i]);
        printf("\nwant:  ");
        for (int i = 0; i < n; ++i) printf("%02X ", expect[i]);
        printf("\n");
    }

    // GDPadCurr32 pads to a 32-byte boundary with zeros.
    GDPadCurr32();
    CHECK(((dl.ptr - dl.start) & 0x1f) == 0);
    for (int i = n; i < (int)(dl.ptr - dl.start); ++i) CHECK(buf[i] == 0);

    printf(g_fail ? "gd_test: %d FAILED\n" : "gd_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
