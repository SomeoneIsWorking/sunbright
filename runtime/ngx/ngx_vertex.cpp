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

}  // namespace

int ngx_extract_positions(float* out, const NgxCP& cp, unsigned vat,
                          const unsigned char* vtx, int count, unsigned vstride,
                          const unsigned char* pos_array, unsigned pos_stride) {
    const unsigned cls   = (cp.vcd_lo >> 9) & 3;          // NotPresent/Direct/Index8/Index16
    const unsigned elems = ((cp.vat[vat][0] >> 0) & 1) ? 3u : 2u;
    const unsigned fmt   = (cp.vat[vat][0] >> 1) & 7;
    const unsigned frac  = (cp.vat[vat][0] >> 4) & 0x1f;
    const unsigned nmtx  = (unsigned)__builtin_popcount(cp.vcd_lo & 0x1FF);  // matrix-index bytes precede pos
    const unsigned csz   = comp_bytes(fmt);
    if (cls == 0) return 0;

    for (int i = 0; i < count; i++) {
        const unsigned char* at = vtx + (size_t)i * vstride + nmtx;
        const unsigned char* src;
        if (cls == 1) {                       // Direct: position inline
            src = at;
        } else if (cls == 2) {                // Index8
            src = pos_array ? pos_array + (size_t)at[0] * pos_stride : nullptr;
        } else {                              // Index16
            src = pos_array ? pos_array + (size_t)be16(at) * pos_stride : nullptr;
        }
        float* o = out + (size_t)i * 3;
        if (!src) { o[0] = o[1] = o[2] = 0; continue; }
        o[0] = read_comp(src, fmt, frac);
        o[1] = read_comp(src + csz, fmt, frac);
        o[2] = (elems == 3) ? read_comp(src + 2 * csz, fmt, frac) : 0.0f;
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

    app("ngx_vertex_selftest: %d cases, %d failing -> %s\n", cases, fails, fails==0?"OK":"MISMATCH");
    return fails;
}
