// j3d_decode.cpp — see j3d_decode.h. Spec: docs/re_notes/j3d_shape_decode.md.

#include "j3d_decode.h"

#include <lucent/log.h>

// j3dSys (US) — the live vertex array bases live here, NOT in the shape (spec §1).
constexpr u32 SB_J3DSYS         = 0x804045DC;
constexpr u32 J3DSYS_POS_ARRAY  = 0x10C;
constexpr u32 J3DSYS_CLR_ARRAY  = 0x114;   // J3DShape::loadVtxArray binds CLR0 from here
// TEX0-7 array bases are STATIC per shape and live in its J3DVertexData, not in j3dSys (only
// POS/NRM/CLR0 are swapped per frame) — J3DShape::loadVtxArray bakes them via GDSetArray.
constexpr u32 SHAPE_VERTEX_DATA_OFF = 0x44;
constexpr u32 VERTEXDATA_TEXCOORD0  = 0x24;
constexpr u32 J3DSYS_NRM_ARRAY  = 0x110;
constexpr u32 J3DSYS_CLR0_ARRAY = 0x114;

namespace {

// J3DShape fields used here.
constexpr u32 SHAPE_UNK8          = 0x08;
constexpr u32 SHAPE_VTX_DESC_LIST = 0x2C;
constexpr u32 SHAPE_DRAWS         = 0x38;
constexpr u32 SHAPE_VERTEX_DATA   = 0x44;
constexpr u32 SHAPE_UNK30         = 0x30;   // bool: normals carry NBT
constexpr u32 VTXDATA_ATTR_FMT    = 0x0C;   // GXVtxAttrFmtList*
constexpr u32 SHAPEDRAW_DL_SIZE   = 0x04;
constexpr u32 SHAPEDRAW_DL_PTR    = 0x08;

// GXCompCnt values, per attribute class.
constexpr u32 NRM_XYZ = 0, NRM_NBT = 1, NRM_NBT3 = 2;

bool ok(u32 p) { return sb_ram_fast(p) != nullptr; }

// Component size in bytes for a non-colour attribute: U8=0,S8=1,U16=2,S16=3,F32=4.
uint32_t comp_size(uint32_t type) {
    switch (type) {
    case 0: case 1: return 1;
    case 2: case 3: return 2;
    case 4: return 4;
    default: return 0;
    }
}

// Packed colour sizes: RGB565=0,RGB8=1,RGBX8=2,RGBA4=3,RGBA6=4,RGBA8=5.
uint32_t colour_size(uint32_t type) {
    switch (type) {
    case 0: case 3: return 2;
    case 1: case 4: return 3;
    case 2: case 5: return 4;
    default: return 0;
    }
}

// How many components an attribute has, given its GXCompCnt.
uint32_t comp_count(uint32_t attr, uint32_t cnt) {
    if (attr == GXA_POS) return cnt ? 3 : 2;                       // XY=0, XYZ=1
    if (attr == GXA_NRM) return (cnt == NRM_XYZ) ? 3 : 9;          // NBT/NBT3 carry 9
    if (attr >= GXA_TEX0 && attr <= GXA_TEX0 + 7) return cnt ? 2 : 1;   // S=0, ST=1
    if (attr == GXA_CLR0 || attr == GXA_CLR1) return 1;            // packed
    return 1;                                                      // the *MTXIDX attrs
}

// Bytes one attribute occupies IN THE VERTEX (not in its array).
uint32_t attr_vertex_bytes(uint32_t attr, const J3DVertexLayout& L) {
    // The nine matrix-index attributes (PNMTXIDX + TEX0-7MTXIDX) are ALWAYS one raw byte when
    // present — GX only permits NONE or DIRECT for them, and their size does not come from the
    // attribute format at all. Running them through the generic path with the default F32 format
    // computed 4 bytes each, making every vertex of a matrix-indexed shape 3+ bytes too long and
    // desyncing its display list (measured: 21% of elements failed exactly this way).
    if (attr <= GXA_TEX0MTXIDX + 7) return (L.type[attr] == GXAT_NONE) ? 0u : 1u;

    switch (L.type[attr]) {
    case GXAT_NONE: return 0;
    case GXAT_DIRECT: {
        if (attr == GXA_CLR0 || attr == GXA_CLR1) return colour_size(L.comp[attr]);
        return comp_size(L.comp[attr]) * comp_count(attr, L.cnt[attr]);
    }
    // An indexed NBT3 normal costs THREE indices, not one — the single GX quirk that makes
    // vertex sizing non-uniform, and the one that desyncs the whole stream if missed.
    case GXAT_INDEX8:  return (attr == GXA_NRM && L.cnt[attr] == NRM_NBT3) ? 3 : 1;
    case GXAT_INDEX16: return (attr == GXA_NRM && L.cnt[attr] == NRM_NBT3) ? 6 : 2;
    default: return 0;
    }
}

} // namespace

bool j3d_build_layout(u32 shape, J3DVertexLayout& L) {
    L = J3DVertexLayout{};
    const u32 descList = sb_r32(shape + SHAPE_VTX_DESC_LIST);
    const u32 vtxData  = sb_r32(shape + SHAPE_VERTEX_DATA);
    if (!ok(descList) || !ok(vtxData)) return false;
    const u32 fmtList = sb_r32(vtxData + VTXDATA_ATTR_FMT);
    if (!ok(fmtList)) return false;

    L.nbt = sb_r8(shape + SHAPE_UNK30) != 0;

    // Defaults for attributes the format list omits (J3DTevs.cpp): POS XYZ/F32, NRM XYZ/F32,
    // CLR RGBA/RGBA8, TEX ST/F32 — all frac 0.
    for (uint32_t a = 0; a < GXA_COUNT; ++a) {
        L.comp[a] = 4;                                   // F32
        L.cnt[a] = (a == GXA_POS) ? 1 : (a == GXA_NRM ? NRM_XYZ : 1);
        if (a == GXA_CLR0 || a == GXA_CLR1) { L.cnt[a] = 1; L.comp[a] = 5; }   // RGBA / RGBA8
    }

    // Descriptor: 8-byte entries {u32 attr, u32 type}, terminated by attr == 0xFF.
    for (u32 e = 0; e < 64; ++e) {
        const u32 p = descList + e * 8;
        if (!ok(p + 7)) break;
        const u32 attr = sb_r32(p);
        if (attr == GXA_NULL) break;
        const u32 type = sb_r32(p + 4);
        if (attr == GXA_NBT) {
            // J3D's marker for "normals carry N+B+T": it occupies the NRM slot in the VAT.
            L.type[GXA_NRM] = (uint8_t)type;
            L.cnt[GXA_NRM] = NRM_NBT;
            L.nbt = true;
        } else if (attr < GXA_COUNT) {
            L.type[attr] = (uint8_t)type;
        }
    }

    // Attribute formats: 16-byte entries {u32 attr, u32 cnt, u32 type, u8 frac}, attr == 0xFF ends.
    for (u32 e = 0; e < 64; ++e) {
        const u32 p = fmtList + e * 16;
        if (!ok(p + 15)) break;
        const u32 attr = sb_r32(p);
        if (attr == GXA_NULL) break;
        const u32 slot = (attr == GXA_NBT) ? GXA_NRM : attr;
        if (slot >= GXA_COUNT) continue;
        L.cnt[slot]  = sb_r32(p + 4);
        L.comp[slot] = sb_r32(p + 8);
        L.frac[slot] = sb_r8(p + 12);
    }
    // NBT override: a shape flagged NBT whose NRM cnt is not NBT3 is forced to NBT (9 components,
    // single index) — J3DTevs.cpp.
    if (L.nbt && L.cnt[GXA_NRM] != NRM_NBT3) L.cnt[GXA_NRM] = NRM_NBT;

    // Offsets accumulate in GXAttr enum order, which is the order attributes appear in a vertex.
    uint32_t off = 0;
    for (uint32_t a = 0; a < GXA_COUNT; ++a) {
        L.offset[a] = off;
        off += attr_vertex_bytes(a, L);
    }
    L.vtxSize = off;
    L.valid = L.vtxSize > 0 && L.type[GXA_POS] != GXAT_NONE;
    return L.valid;
}

bool j3d_decode_element(u32 shape, uint32_t element, const J3DVertexLayout& L,
                        std::vector<J3DVert>& out) {
    if (!L.valid) return false;
    const u32 draws = sb_r32(shape + SHAPE_DRAWS);
    if (!ok(draws)) return false;
    const u32 d = sb_r32(draws + element * 4);
    if (!ok(d)) return false;
    const u32 dl   = sb_r32(d + SHAPEDRAW_DL_PTR);
    const u32 size = sb_r32(d + SHAPEDRAW_DL_SIZE);
    if (!ok(dl) || size == 0 || size > (16u << 20)) return false;

    // Positions come from the LIVE array base in j3dSys, not the shape's own — they are swapped
    // per frame for skinning/colour animation (spec §1). Reading the shape's would silently render
    // stale geometry, which is the worst kind of wrong: plausible.
    const u32 posBase = sb_r32(SB_J3DSYS + J3DSYS_POS_ARRAY);
    if (!ok(posBase)) return false;
    // J3D's baked stride: 12 bytes if F32, else 6 (makeVtxArrayCmd).
    const uint32_t posStride = (L.comp[GXA_POS] == 4) ? 12u : 6u;
    const float posScale = 1.0f / (float)(1u << L.frac[GXA_POS]);

    // CLR0 (live base) and TEX0 (per-shape base). Both optional: a shape without the attribute
    // gets opaque white / zero UV, which is what an untextured, unlit material draws as.
    const u32 clrBase = (L.type[GXA_CLR0] != GXAT_NONE) ? sb_r32(SB_J3DSYS + J3DSYS_CLR_ARRAY) : 0;
    const uint32_t clrStride = colour_size(L.comp[GXA_CLR0]);
    const u32 vtxData = sb_r32(shape + SHAPE_VERTEX_DATA_OFF);
    // J3DVertexData::mVtxTexCoordArray[8] at +0x24 — one base per coordinate set, each with its
    // own format and fractional shift. 72% of the plaza's drawables reference more than one, so
    // reading only TEX0 gave every extra stage the first set's coordinates.
    u32      texBase[J3D_TEXCOORD_SETS]{};
    uint32_t texStride[J3D_TEXCOORD_SETS]{};
    float    texScale[J3D_TEXCOORD_SETS]{};
    for (uint32_t s = 0; s < J3D_TEXCOORD_SETS; ++s) {
        const uint32_t attr = GXA_TEX0 + s;
        texBase[s] = (L.type[attr] != GXAT_NONE && ok(vtxData))
                         ? sb_r32(vtxData + VERTEXDATA_TEXCOORD0 + s * 4) : 0;
        // J3D's baked stride for a 2-component texcoord: 8 bytes if F32, else 4 (two s16).
        texStride[s] = (L.comp[attr] == 4) ? 8u : 4u;
        texScale[s]  = 1.0f / (float)(1u << L.frac[attr]);
    }
    // A set beyond what this decoder reads would silently sample set 0 instead, which looks like a
    // UV bug rather than a missing decode — so say so once (CLAUDE.md: no silent stubs).
    for (uint32_t s = J3D_TEXCOORD_SETS; s < 8; ++s) {
        if (L.type[GXA_TEX0 + s] == GXAT_NONE) continue;
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("j3d", "shape 0x{:08x} declares TEX{} — this decoder reads {} coordinate "
                                 "sets, so that set is NOT available to its texgen",
                          shape, s, J3D_TEXCOORD_SETS);
        }
    }

    // Normals share the position array's convention: J3D bakes a 12-byte stride for F32 and 6 for
    // S16 (three components), with the format's fractional shift applied.
    const u32 nrmBase = (L.type[GXA_NRM] != GXAT_NONE) ? sb_r32(SB_J3DSYS + J3DSYS_NRM_ARRAY) : 0;
    const uint32_t nrmStride = (L.comp[GXA_NRM] == 4) ? 12u : 6u;
    const float nrmScale = 1.0f / (float)(1u << L.frac[GXA_NRM]);

    auto read_nrm = [&](uint32_t index, J3DVert& v) {
        if (nrmBase == 0) return;
        const u32 p = nrmBase + index * nrmStride;
        if (!ok(p + nrmStride - 1)) return;
        float c[3] = {0, 0, 0};
        for (uint32_t k = 0; k < 3; ++k) {
            if (L.comp[GXA_NRM] == 4) {
                const u32 bits = sb_r32(p + k * 4);
                __builtin_memcpy(&c[k], &bits, 4);
            } else {
                c[k] = (float)(int16_t)sb_r16(p + k * 2) * nrmScale;
            }
        }
        v.nx = c[0]; v.ny = c[1]; v.nz = c[2];
    };

    auto read_clr = [&](uint32_t index, J3DVert& v) {
        if (clrBase == 0) return;
        const u32 p = clrBase + index * clrStride;
        if (!ok(p + clrStride - 1)) return;
        if (clrStride == 4) {
            v.rgba = sb_r32(p);                       // RGBA8
        } else {
            const uint16_t c = sb_r16(p);             // RGB565 / RGB5A3
            if (c & 0x8000) {                         // RGB5A3 opaque: 5/5/5
                const uint32_t r = (c >> 10) & 0x1F, g = (c >> 5) & 0x1F, b = c & 0x1F;
                v.rgba = (r * 255 / 31) << 24 | (g * 255 / 31) << 16 | (b * 255 / 31) << 8 | 0xFF;
            } else {                                  // RGB5A3 translucent: 3 alpha, 4/4/4
                const uint32_t a = (c >> 12) & 0x7, r = (c >> 8) & 0xF, g = (c >> 4) & 0xF,
                               b = c & 0xF;
                v.rgba = (r * 255 / 15) << 24 | (g * 255 / 15) << 16 | (b * 255 / 15) << 8 |
                         (a * 255 / 7);
            }
        }
    };

    auto read_tex = [&](uint32_t set, uint32_t index, J3DVert& v) {
        if (set >= J3D_TEXCOORD_SETS || texBase[set] == 0) return;
        const u32 p = texBase[set] + index * texStride[set];
        if (!ok(p + texStride[set] - 1)) return;
        if (texStride[set] == 8) {
            u32 b0 = sb_r32(p), b1 = sb_r32(p + 4);
            __builtin_memcpy(&v.uv[set][0], &b0, 4);
            __builtin_memcpy(&v.uv[set][1], &b1, 4);
        } else {
            v.uv[set][0] = (float)(int16_t)sb_r16(p) * texScale[set];
            v.uv[set][1] = (float)(int16_t)sb_r16(p + 2) * texScale[set];
        }
    };

    auto read_pos = [&](uint32_t index, J3DVert& v) -> bool {
        const u32 p = posBase + index * posStride;
        if (!ok(p + posStride - 1)) return false;
        const uint32_t n = comp_count(GXA_POS, L.cnt[GXA_POS]);
        float c[3] = {0, 0, 0};
        for (uint32_t k = 0; k < n && k < 3; ++k) {
            if (L.comp[GXA_POS] == 4) {
                const u32 bits = sb_r32(p + k * 4);
                __builtin_memcpy(&c[k], &bits, 4);
            } else {
                // S16 is the only non-F32 position format SMS uses; sign-extend then apply frac.
                c[k] = (float)(int16_t)sb_r16(p + k * 2) * posScale;
            }
        }
        v.x = c[0]; v.y = c[1]; v.z = c[2];
        return true;
    };

    std::vector<J3DVert> prim;
    u32 pos = 0;
    while (pos < size) {
        const u8 cmd = sb_r8(dl + pos);
        ++pos;
        if (cmd == 0x00) continue;                       // NOP / trailing padding
        const u8 op = cmd & 0xF8;
        const bool isPrim = (op == 0x80 || op == 0x90 || op == 0x98 || op == 0xA0 ||
                             op == 0xA8 || op == 0xB0 || op == 0xB8);
        if (!isPrim) {
            // A foreign opcode means the vertex size is wrong and we are mid-payload. Loud, because
            // continuing renders garbage that looks like a shading bug.
            // Report the LAYOUT and the size that would have fit, not just "desync" — the useful
            // question is which attribute is mis-sized, and the first primitive answers it:
            // for a single-primitive list, trueSize ~= (bytesBeforePadding - 3) / nverts.
            static int s_diag = 0;
            if (s_diag < 6) {
                ++s_diag;
                const uint32_t n0 = sb_r16(dl + 1);          // first primitive's vertex count
                lucent::error("j3d", "desync shape 0x{:08x} el {} at +{} opcode 0x{:02x}; "
                                     "vtxSize={} dlSize={} firstPrimVerts={} -> a size of ~{} "
                                     "would fit",
                              shape, element, pos - 1, cmd, L.vtxSize, size, n0,
                              n0 ? (size - 3) / n0 : 0);
                for (uint32_t a = 0; a < GXA_COUNT; ++a)
                    if (L.type[a] != GXAT_NONE)
                        lucent::error("j3d", "   attr {:2} type {} cnt {} comp {} frac {} -> {} B",
                                      a, L.type[a], L.cnt[a], L.comp[a], L.frac[a],
                                      attr_vertex_bytes(a, L));
            }
            return false;
        }
        if (pos + 2 > size) return false;
        const uint32_t nverts = sb_r16(dl + pos);
        pos += 2;
        const uint32_t payload = nverts * L.vtxSize;
        if (pos + payload > size) {
            lucent::error("j3d", "display-list overrun in shape 0x{:08x} element {}: {} verts x {} "
                                 "bytes exceeds {} remaining",
                          shape, element, nverts, L.vtxSize, size - pos);
            return false;
        }

        prim.clear();
        prim.reserve(nverts);
        for (uint32_t v = 0; v < nverts; ++v) {
            const u32 vp = dl + pos + v * L.vtxSize;
            J3DVert out_v{};
            // The matrix slot: PNMTXIDX is the GX matrix ADDRESS, and slot = address / 3.
            out_v.pnMtxSlot = 0;
            if (L.type[GXA_PNMTXIDX] == GXAT_DIRECT)
                out_v.pnMtxSlot = sb_r8(vp + L.offset[GXA_PNMTXIDX]) / 3u;
            uint32_t index = 0;
            const u32 pp = vp + L.offset[GXA_POS];
            if (L.type[GXA_POS] == GXAT_INDEX16)      index = sb_r16(pp);
            else if (L.type[GXA_POS] == GXAT_INDEX8)  index = sb_r8(pp);
            else { /* DIRECT positions are inline — rare in BMD, handled below */ }

            if (L.type[GXA_POS] == GXAT_DIRECT) {
                const uint32_t n = comp_count(GXA_POS, L.cnt[GXA_POS]);
                for (uint32_t k = 0; k < n && k < 3; ++k) {
                    float c = 0;
                    if (L.comp[GXA_POS] == 4) {
                        const u32 bits = sb_r32(pp + k * 4);
                        __builtin_memcpy(&c, &bits, 4);
                    } else {
                        c = (float)(int16_t)sb_r16(pp + k * 2) * posScale;
                    }
                    (&out_v.x)[k] = c;
                }
            } else if (!read_pos(index, out_v)) {
                return false;
            }

            if (L.type[GXA_NRM] == GXAT_INDEX16)     read_nrm(sb_r16(vp + L.offset[GXA_NRM]), out_v);
            else if (L.type[GXA_NRM] == GXAT_INDEX8) read_nrm(sb_r8(vp + L.offset[GXA_NRM]), out_v);

            // Colour and texcoord, each with its own index in the vertex.
            out_v.rgba = 0xFFFFFFFFu;
            if (L.type[GXA_CLR0] == GXAT_INDEX16)     read_clr(sb_r16(vp + L.offset[GXA_CLR0]), out_v);
            else if (L.type[GXA_CLR0] == GXAT_INDEX8) read_clr(sb_r8(vp + L.offset[GXA_CLR0]), out_v);
            else if (L.type[GXA_CLR0] == GXAT_DIRECT) out_v.rgba = sb_r32(vp + L.offset[GXA_CLR0]);

            // Every declared coordinate set, each with its own index/direct payload in the vertex.
            for (uint32_t s = 0; s < J3D_TEXCOORD_SETS; ++s) {
                const uint32_t attr = GXA_TEX0 + s;
                const u32 off = vp + L.offset[attr];
                if (L.type[attr] == GXAT_INDEX16)     read_tex(s, sb_r16(off), out_v);
                else if (L.type[attr] == GXAT_INDEX8) read_tex(s, sb_r8(off), out_v);
                else if (L.type[attr] == GXAT_DIRECT) {
                    if (L.comp[attr] == 4) {
                        u32 b0 = sb_r32(off), b1 = sb_r32(off + 4);
                        __builtin_memcpy(&out_v.uv[s][0], &b0, 4);
                        __builtin_memcpy(&out_v.uv[s][1], &b1, 4);
                    } else {
                        out_v.uv[s][0] = (float)(int16_t)sb_r16(off) * texScale[s];
                        out_v.uv[s][1] = (float)(int16_t)sb_r16(off + 2) * texScale[s];
                    }
                }
            }
            prim.push_back(out_v);
        }
        pos += payload;

        // Triangulate into `out` (spec §3).
        const size_t n = prim.size();
        auto emit = [&](size_t a, size_t b, size_t c) {
            out.push_back(prim[a]); out.push_back(prim[b]); out.push_back(prim[c]);
        };
        if (op == 0x90) {                        // TRIANGLES
            for (size_t v = 0; v + 2 < n; v += 3) emit(v, v + 1, v + 2);
        } else if (op == 0x98) {                 // TRIANGLESTRIP
            for (size_t v = 2; v < n; ++v)
                if (v & 1) emit(v - 1, v - 2, v); else emit(v - 2, v - 1, v);
        } else if (op == 0xA0) {                 // TRIANGLEFAN
            for (size_t v = 2; v < n; ++v) emit(0, v - 1, v);
        } else if (op == 0x80) {                 // QUADS
            for (size_t v = 0; v + 3 < n; v += 4) { emit(v, v + 1, v + 2); emit(v + 2, v + 3, v); }
        }
        // LINES/LINESTRIP/POINTS do not appear in BMD shape lists; consumed above, not emitted.
    }
    return true;
}
