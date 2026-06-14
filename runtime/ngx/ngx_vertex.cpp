// ngx_vertex — see ngx_vertex.h. Native GC vertex-position extraction, no Dolphin.

#include "ngx_vertex.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

inline unsigned be16(const unsigned char* p) { return (unsigned)((p[0] << 8) | p[1]); }
inline float bef32(const unsigned char* p) {
    unsigned u = ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
    float f; std::memcpy(&f, &u, 4); return f;
}

// One position component (big-endian) → float, with fixed-point dequant for
// integer formats. fmt: 0=u8,1=s8,2=u16,3=s16,4..7=float.
inline float read_comp(const unsigned char* p, unsigned fmt, unsigned frac) {
    const float scale = (fmt >= 4) ? 1.0f : 1.0f / (float)(1u << frac);
    switch (fmt) {
    case 0: return (float)(unsigned)p[0] * scale;             // u8
    case 1: return (float)(int)(signed char)p[0] * scale;     // s8
    case 2: return (float)(unsigned)be16(p) * scale;          // u16
    case 3: return (float)(short)be16(p) * scale;             // s16
    default: return bef32(p);                                 // float (5/6/7 == float)
    }
}
inline unsigned comp_bytes(unsigned fmt) { return (fmt >= 4) ? 4u : (fmt >= 2 ? 2u : 1u); }

// One normal component → float. GC normals use a FIXED fractional scale by
// component format (no programmable frac): signed/unsigned binary point such that
// the value spans roughly [-1,1). Matches Dolphin VertexLoader_Normal::FracAdjust
// (val / 2^(bits-signed-1)). fmt: 0=u8,1=s8,2=u16,3=s16,4..7=float.
inline float read_nrm_comp(const unsigned char* p, unsigned fmt) {
    switch (fmt) {
    case 0: return (float)(unsigned)p[0] / 128.0f;          // u8  /2^7
    case 1: return (float)(int)(signed char)p[0] / 64.0f;   // s8  /2^6
    case 2: return (float)(unsigned)be16(p) / 32768.0f;     // u16 /2^15
    case 3: return (float)(short)be16(p) / 16384.0f;        // s16 /2^14
    default: return bef32(p);                               // float
    }
}

// Decode one GC vertex color (src already resolved to its bytes) to an
// AABBGGRR u32. Endianness handling per format mirrors Dolphin's
// VertexLoader_Color exactly (565/6666 read big-endian; 4444 reads the raw
// little-endian 16-bit "BARG"; 8888/888 take bytes in R,G,B[,A] order).
inline unsigned decode_color(const unsigned char* s, unsigned fmt) {
    auto rgb = [](unsigned r, unsigned g, unsigned b, unsigned a) {
        return r | (g << 8) | (b << 16) | (a << 24);
    };
    switch (fmt) {
    case 0: {  // RGB565 (big-endian u16)
        unsigned v = be16(s), col;
        col  = (v >> 8) & 0x0000F8u;
        col |= (v << 5) & 0x00FC00u;
        col |= (v << 19) & 0xF80000u;
        col |= (col >> 5) & 0x070007u;
        col |= (col >> 6) & 0x000300u;
        return col | 0xFF000000u;
    }
    case 1:   // RGB888
    case 2:   // RGB888x
        return rgb(s[0], s[1], s[2], 0xFF);
    case 3: { // RGBA4444 (raw little-endian u16, "BARG")
        unsigned v = (unsigned)s[0] | ((unsigned)s[1] << 8), col;
        col  = v & 0x00F0u;
        col |= (v & 0x000Fu) << 12;
        col |= (v & 0xF000u) << 8;
        col |= (v & 0x0F00u) << 20;
        col |= col >> 4;
        return col;
    }
    case 4: { // RGBA6666 (big-endian u24)
        unsigned v = ((unsigned)s[0] << 16) | ((unsigned)s[1] << 8) | s[2], col;
        col  = (v >> 16) & 0x000000FCu;
        col |= (v >> 2)  & 0x0000FC00u;
        col |= (v << 12) & 0x00FC0000u;
        col |= (v << 26) & 0xFC000000u;
        col |= (col >> 6) & 0x03030303u;
        return col;
    }
    default:  // RGBA8888
        return rgb(s[0], s[1], s[2], s[3]);
    }
}

// Per-channel tex-coord VAT field accessors (format / elements / frac), threading
// VAT_A/B/C exactly as Dolphin's VAT::GetTexFormat/Elements/Frac (CPMemory.h).
inline unsigned tex_fmt(const NgxCP& cp, unsigned vat, int i) {
    switch (i) {
    case 0: return (cp.vat[vat][0] >> 22) & 7;
    case 1: return (cp.vat[vat][1] >> 1)  & 7;
    case 2: return (cp.vat[vat][1] >> 10) & 7;
    case 3: return (cp.vat[vat][1] >> 19) & 7;
    case 4: return (cp.vat[vat][1] >> 28) & 7;
    case 5: return (cp.vat[vat][2] >> 6)  & 7;
    case 6: return (cp.vat[vat][2] >> 15) & 7;
    default:return (cp.vat[vat][2] >> 24) & 7;
    }
}
inline unsigned tex_elem(const NgxCP& cp, unsigned vat, int i) {
    switch (i) {
    case 0: return (cp.vat[vat][0] >> 21) & 1;
    case 1: return (cp.vat[vat][1] >> 0)  & 1;
    case 2: return (cp.vat[vat][1] >> 9)  & 1;
    case 3: return (cp.vat[vat][1] >> 18) & 1;
    case 4: return (cp.vat[vat][1] >> 27) & 1;
    case 5: return (cp.vat[vat][2] >> 5)  & 1;
    case 6: return (cp.vat[vat][2] >> 14) & 1;
    default:return (cp.vat[vat][2] >> 23) & 1;
    }
}
inline unsigned tex_frac(const NgxCP& cp, unsigned vat, int i) {
    switch (i) {
    case 0: return (cp.vat[vat][0] >> 25) & 0x1f;
    case 1: return (cp.vat[vat][1] >> 4)  & 0x1f;
    case 2: return (cp.vat[vat][1] >> 13) & 0x1f;
    case 3: return (cp.vat[vat][1] >> 22) & 0x1f;
    case 4: return (cp.vat[vat][2] >> 0)  & 0x1f;
    case 5: return (cp.vat[vat][2] >> 9)  & 0x1f;
    case 6: return (cp.vat[vat][2] >> 18) & 0x1f;
    default:return (cp.vat[vat][2] >> 27) & 0x1f;
    }
}

// Resolve the host pointer to an attribute's bytes for vertex `i`: Direct →
// inline at the attribute offset; Index8/16 → array base + index*stride.
// Returns null if indexed with no array. `*idx_bytes` (optional) reports the
// index size consumed (0 for Direct) — unused here but documents the layout.
inline const unsigned char* resolve_attr(unsigned cls, const unsigned char* at,
                                         const unsigned char* arr, unsigned stride) {
    if (cls == 1) return at;                                       // Direct
    if (cls == 2) return arr ? arr + (size_t)at[0] * stride : nullptr;     // Index8
    return arr ? arr + (size_t)be16(at) * stride : nullptr;               // Index16
}

}  // namespace

int ngx_extract_positions(float* out, const NgxCP& cp, unsigned vat,
                          const unsigned char* vtx, int count, unsigned vstride,
                          const unsigned char* pos_array, unsigned pos_stride) {
    const unsigned cls   = (cp.vcd_lo >> 9) & 3;          // NotPresent/Direct/Index8/Index16
    if (cls == 0) return 0;
    const unsigned elems = ((cp.vat[vat][0] >> 0) & 1) ? 3u : 2u;
    const unsigned fmt   = (cp.vat[vat][0] >> 1) & 7;
    const unsigned frac  = (cp.vat[vat][0] >> 4) & 0x1f;
    const unsigned off   = ngx_attr_offset(cp, vat, NGX_POS);  // matrix-index bytes precede pos
    const unsigned csz   = comp_bytes(fmt);

    for (int i = 0; i < count; i++) {
        const unsigned char* at  = vtx + (size_t)i * vstride + off;
        const unsigned char* src = resolve_attr(cls, at, pos_array, pos_stride);
        float* o = out + (size_t)i * 3;
        if (!src) { o[0] = o[1] = o[2] = 0; continue; }
        o[0] = read_comp(src, fmt, frac);
        o[1] = read_comp(src + csz, fmt, frac);
        o[2] = (elems == 3) ? read_comp(src + 2 * csz, fmt, frac) : 0.0f;
    }
    return count;
}

int ngx_extract_normals(float* out, const NgxCP& cp, unsigned vat,
                        const unsigned char* vtx, int count, unsigned vstride,
                        const unsigned char* nrm_array, unsigned nrm_stride) {
    const unsigned cls = (cp.vcd_lo >> 11) & 3;
    if (cls == 0) return 0;
    const unsigned fmt = (cp.vat[vat][0] >> 10) & 7;       // NormalFormat
    const unsigned off = ngx_attr_offset(cp, vat, NGX_NRM);
    const unsigned csz = comp_bytes(fmt);
    // NTB+NormalIndex3 packs 3 indices (N,T,B); the N index is first, so reading
    // index[0] and its 3 components yields N for every indexed layout.

    for (int i = 0; i < count; i++) {
        const unsigned char* at  = vtx + (size_t)i * vstride + off;
        const unsigned char* src = resolve_attr(cls, at, nrm_array, nrm_stride);
        float* o = out + (size_t)i * 3;
        if (!src) { o[0] = o[1] = o[2] = 0; continue; }
        o[0] = read_nrm_comp(src, fmt);
        o[1] = read_nrm_comp(src + csz, fmt);
        o[2] = read_nrm_comp(src + 2 * csz, fmt);
    }
    return count;
}

int ngx_extract_colors(unsigned char* out, const NgxCP& cp, unsigned vat, int chan,
                       const unsigned char* vtx, int count, unsigned vstride,
                       const unsigned char* col_array, unsigned col_stride) {
    const unsigned cls = (cp.vcd_lo >> (13 + 2 * (chan & 1))) & 3;
    if (cls == 0) return 0;
    const unsigned fmt = (cp.vat[vat][0] >> (14 + 4 * (chan & 1))) & 7;  // Color0/1 Comp
    const int attr     = NGX_CLR0 + (chan & 1);
    const unsigned off = ngx_attr_offset(cp, vat, attr);

    for (int i = 0; i < count; i++) {
        const unsigned char* at  = vtx + (size_t)i * vstride + off;
        const unsigned char* src = resolve_attr(cls, at, col_array, col_stride);
        unsigned char* o = out + (size_t)i * 4;
        if (!src) { o[0] = o[1] = o[2] = 0; o[3] = 0xFF; continue; }
        unsigned col = decode_color(src, fmt);
        o[0] = col & 0xFF; o[1] = (col >> 8) & 0xFF;
        o[2] = (col >> 16) & 0xFF; o[3] = (col >> 24) & 0xFF;
    }
    return count;
}

int ngx_extract_texcoords(float* out, const NgxCP& cp, unsigned vat, int chan,
                          const unsigned char* vtx, int count, unsigned vstride,
                          const unsigned char* tex_array, unsigned tex_stride) {
    const unsigned cls = (cp.vcd_hi >> (2 * (chan & 7))) & 3;
    if (cls == 0) return 0;
    const unsigned elems = tex_elem(cp, vat, chan & 7) ? 2u : 1u;       // 0=S,1=ST
    const unsigned fmt   = tex_fmt(cp, vat, chan & 7);
    const unsigned frac  = tex_frac(cp, vat, chan & 7);
    const int attr       = NGX_TEX0 + (chan & 7);
    const unsigned off   = ngx_attr_offset(cp, vat, attr);
    const unsigned csz   = comp_bytes(fmt);

    for (int i = 0; i < count; i++) {
        const unsigned char* at  = vtx + (size_t)i * vstride + off;
        const unsigned char* src = resolve_attr(cls, at, tex_array, tex_stride);
        float* o = out + (size_t)i * 2;
        if (!src) { o[0] = o[1] = 0; continue; }
        o[0] = read_comp(src, fmt, frac);
        o[1] = (elems == 2) ? read_comp(src + csz, fmt, frac) : 0.0f;
    }
    return count;
}

// ── Self-test (hand-constructed inputs, known expected positions) ───────────────
namespace {
struct Case { const char* name; };

bool approx(float a, float b) { float d = a - b; if (d < 0) d = -d; return d <= 1e-4f * (1 + (b < 0 ? -b : b)); }

// Build an NgxCP with only position configured (no matrix indices, no other attrs).
NgxCP mkcp(unsigned cls, unsigned elemsXYZ, unsigned fmt, unsigned frac) {
    NgxCP cp{};
    cp.vcd_lo = (cls & 3) << 9;
    cp.vat[0][0] = (elemsXYZ & 1) | ((fmt & 7) << 1) | ((frac & 0x1f) << 4);
    return cp;
}
}  // namespace

int sb_ngx_vertex_selftest(char* outbuf, int cap) {
    int pos = 0, fails = 0, cases = 0;
    auto app = [&](const char* f, ...) { if (pos >= cap) return;
        va_list a; va_start(a, f); pos += vsnprintf(outbuf + pos, cap - pos, f, a); va_end(a); };
    auto put_be16 = [](unsigned char* p, int v) { p[0] = (v >> 8) & 0xff; p[1] = v & 0xff; };
    auto put_bef32 = [](unsigned char* p, float f) { unsigned u; std::memcpy(&u, &f, 4);
        p[0]=u>>24;p[1]=u>>16;p[2]=u>>8;p[3]=u; };
    auto check = [&](const char* name, float gx, float gy, float gz, float ex, float ey, float ez) {
        cases++;
        if (!approx(gx,ex) || !approx(gy,ey) || !approx(gz,ez)) {
            fails++; app("FAIL %-22s got(%.4f,%.4f,%.4f) exp(%.4f,%.4f,%.4f)\n", name, gx,gy,gz, ex,ey,ez);
        }
    };
    unsigned char buf[64]; float o[3];

    // 1. Direct float XYZ
    { NgxCP cp = mkcp(1, 1, 4, 0);
      put_bef32(buf, 1.0f); put_bef32(buf+4, -2.5f); put_bef32(buf+8, 3.25f);
      ngx_extract_positions(o, cp, 0, buf, 1, 12, nullptr, 0);
      check("direct float xyz", o[0],o[1],o[2], 1.0f,-2.5f,3.25f); }

    // 2. Direct s16 XYZ frac=8  (256 -> 1.0)
    { NgxCP cp = mkcp(1, 1, 3, 8);
      put_be16(buf, 256); put_be16(buf+2, -128 & 0xffff); put_be16(buf+4, 512);
      ngx_extract_positions(o, cp, 0, buf, 1, 6, nullptr, 0);
      check("direct s16 xyz frac8", o[0],o[1],o[2], 1.0f,-0.5f,2.0f); }

    // 3. Direct u8 XY frac=0 (z=0)
    { NgxCP cp = mkcp(1, 0, 0, 0);
      buf[0]=10; buf[1]=20;
      ngx_extract_positions(o, cp, 0, buf, 1, 2, nullptr, 0);
      check("direct u8 xy", o[0],o[1],o[2], 10.0f,20.0f,0.0f); }

    // 4. Index16 float XYZ — vtx holds index 1 into a 2-entry array
    { NgxCP cp = mkcp(3, 1, 4, 0);
      unsigned char arr[24];
      put_bef32(arr,9); put_bef32(arr+4,9); put_bef32(arr+8,9);           // entry 0
      put_bef32(arr+12,4.0f); put_bef32(arr+16,5.0f); put_bef32(arr+20,6.0f); // entry 1
      put_be16(buf, 1);
      ngx_extract_positions(o, cp, 0, buf, 1, 2, arr, 12);
      check("index16 float xyz", o[0],o[1],o[2], 4.0f,5.0f,6.0f); }

    // 5. Index8 s16 XYZ frac=4 — index 2, value 16 -> 1.0
    { NgxCP cp = mkcp(2, 1, 3, 4);
      unsigned char arr[24]; std::memset(arr,0,sizeof arr);
      put_be16(arr+12, 16); put_be16(arr+14, 32); put_be16(arr+16, -16 & 0xffff);  // entry 2 (stride 6)
      buf[0] = 2;
      ngx_extract_positions(o, cp, 0, buf, 1, 1, arr, 6);
      check("index8 s16 xyz frac4", o[0],o[1],o[2], 1.0f,2.0f,-1.0f); }

    // 6. Two direct float XYZ vertices (stride/iteration)
    { NgxCP cp = mkcp(1, 1, 4, 0);
      put_bef32(buf,1.0f); put_bef32(buf+4,2.0f); put_bef32(buf+8,3.0f);
      put_bef32(buf+12,4.0f); put_bef32(buf+16,5.0f); put_bef32(buf+20,6.0f);
      float oo[6]; ngx_extract_positions(oo, cp, 0, buf, 2, 12, nullptr, 0);
      check("two-vtx [0]", oo[0],oo[1],oo[2], 1,2,3);
      check("two-vtx [1]", oo[3],oo[4],oo[5], 4,5,6); }

    auto checkc = [&](const char* name, const unsigned char* g,
                      int er, int eg, int eb, int ea) {
        cases++;
        if (g[0]!=er || g[1]!=eg || g[2]!=eb || g[3]!=ea) {
            fails++; app("FAIL %-22s got(%d,%d,%d,%d) exp(%d,%d,%d,%d)\n", name,
                         g[0],g[1],g[2],g[3], er,eg,eb,ea);
        }
    };
    unsigned char cb[4]; float o2[2];

    // 7. Multi-attribute vertex — verifies per-attribute offsets (mtx idx, pos
    //    float xyz @1, normal s16 @13, color rgba8888 @19, tex0 float st @23).
    { NgxCP cp{};
      cp.vcd_lo = (1u<<0) | (1u<<9) | (1u<<11) | (1u<<13);   // PosMtxIdx,Pos,Nrm,Col0 Direct
      cp.vcd_hi = 1u;                                         // Tex0 Direct
      cp.vat[0][0] = (1u<<0)|(4u<<1)|(0u<<9)|(3u<<10)|(5u<<14)|(1u<<21)|(4u<<22);
      unsigned char vb[40] = {0};
      vb[0] = 7;                                              // pos-mtx idx (ignored)
      put_bef32(vb+1,1.0f); put_bef32(vb+5,2.0f); put_bef32(vb+9,3.0f);
      put_be16(vb+13,16384); put_be16(vb+15,(-16384)&0xffff); put_be16(vb+17,8192);
      vb[19]=10; vb[20]=20; vb[21]=30; vb[22]=40;
      put_bef32(vb+23,0.25f); put_bef32(vb+27,0.75f);
      ngx_extract_positions(o, cp, 0, vb, 1, 31, nullptr, 0);
      check("multiattr pos", o[0],o[1],o[2], 1,2,3);
      ngx_extract_normals(o, cp, 0, vb, 1, 31, nullptr, 0);
      check("multiattr nrm s16", o[0],o[1],o[2], 1.0f,-1.0f,0.5f);
      ngx_extract_colors(cb, cp, 0, 0, vb, 1, 31, nullptr, 0);
      checkc("multiattr col 8888", cb, 10,20,30,40);
      ngx_extract_texcoords(o2, cp, 0, 0, vb, 1, 31, nullptr, 0);
      check("multiattr tex0 st", o2[0],o2[1],0, 0.25f,0.75f,0); }

    // 8. Color formats (color-only CP, offset 0). 565 white / 4444 / 6666 white.
    { NgxCP cc{}; cc.vcd_lo = (1u<<13); cc.vat[0][0] = (0u<<14);   // 565 Direct
      buf[0]=0xFF; buf[1]=0xFF; ngx_extract_colors(cb, cc, 0, 0, buf, 1, 2, nullptr, 0);
      checkc("col 565 white", cb, 255,255,255,255); }
    { NgxCP cc{}; cc.vcd_lo = (1u<<13); cc.vat[0][0] = (3u<<14);   // RGBA4444 Direct
      buf[0]=0xF0; buf[1]=0x00; ngx_extract_colors(cb, cc, 0, 0, buf, 1, 2, nullptr, 0);
      checkc("col 4444 R", cb, 255,0,0,0); }
    { NgxCP cc{}; cc.vcd_lo = (1u<<13); cc.vat[0][0] = (4u<<14);   // RGBA6666 Direct
      buf[0]=0xFF; buf[1]=0xFF; buf[2]=0xFF;
      ngx_extract_colors(cb, cc, 0, 0, buf, 1, 3, nullptr, 0);
      checkc("col 6666 white", cb, 255,255,255,255); }

    // 9. Tex direct s16 frac8 (256 -> 1.0, -128 -> -0.5)
    { NgxCP td{}; td.vcd_hi = 1u; td.vat[0][0] = (1u<<21)|(3u<<22)|(8u<<25);
      put_be16(buf,256); put_be16(buf+2,(-128)&0xffff);
      ngx_extract_texcoords(o2, td, 0, 0, buf, 1, 4, nullptr, 0);
      check("tex s16 frac8 st", o2[0],o2[1],0, 1.0f,-0.5f,0); }

    // 10. Indexed normal (Index8 s16, entry 3, stride 6)
    { NgxCP ni{}; ni.vcd_lo = (2u<<11); ni.vat[0][0] = (3u<<10);
      unsigned char narr[24] = {0};
      put_be16(narr+18,16384); put_be16(narr+20,16384); put_be16(narr+22,(-16384)&0xffff);
      buf[0]=3; ngx_extract_normals(o, ni, 0, buf, 1, 1, narr, 6);
      check("index8 nrm s16", o[0],o[1],o[2], 1.0f,1.0f,-1.0f); }

    // 11. Indexed color (Index16 RGBA8888, entry 2, stride 4)
    { NgxCP ci{}; ci.vcd_lo = (3u<<13); ci.vat[0][0] = (5u<<14);
      unsigned char carr[16] = {0}; carr[8]=1; carr[9]=2; carr[10]=3; carr[11]=4;
      put_be16(buf,2); ngx_extract_colors(cb, ci, 0, 0, buf, 1, 2, carr, 4);
      checkc("index16 col 8888", cb, 1,2,3,4); }

    // 12. Indexed texcoord (Index8 float ST, entry 1, stride 8)
    { NgxCP ti{}; ti.vcd_hi = 2u; ti.vat[0][0] = (1u<<21)|(4u<<22);
      unsigned char tarr[16]; put_bef32(tarr+8,0.5f); put_bef32(tarr+12,0.25f);
      buf[0]=1; ngx_extract_texcoords(o2, ti, 0, 0, buf, 1, 1, tarr, 8);
      check("index8 tex float st", o2[0],o2[1],0, 0.5f,0.25f,0); }

    app("ngx_vertex_selftest: %d cases, %d failing -> %s\n", cases, fails, fails==0?"OK":"MISMATCH");
    return fails;
}
