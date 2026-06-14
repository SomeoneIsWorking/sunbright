// ngx_mesh — see ngx_mesh.h. Native mesh assembly + GX primitive triangulation.

#include "ngx_mesh.h"
#include "ngx_vertex.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

// Append a triangle (local vertex indices) referencing the appended vertices.
inline void tri(std::vector<unsigned>& idx, unsigned base, unsigned a, unsigned b, unsigned c) {
    idx.push_back(base + a); idx.push_back(base + b); idx.push_back(base + c);
}

}  // namespace

int ngx_assemble_primitive(const NgxCP& cp, unsigned op,
                           const unsigned char* vtx, int count,
                           const NgxArrays& arr,
                           std::vector<NgxVertex>& verts,
                           std::vector<unsigned>& indices) {
    if (count <= 0) return 0;
    const unsigned vat     = op & 0x07;
    const unsigned vstride = ngx_vertex_size(cp, vat);
    const unsigned base    = (unsigned)verts.size();

    // Append `count` zeroed vertices (color alpha defaults to opaque).
    verts.resize(base + (size_t)count);
    for (int i = 0; i < count; i++) {
        NgxVertex& v = verts[base + i];
        std::memset(&v, 0, sizeof v);
        // Absent vertex-color channels default to WHITE, not black: in the common
        // texColor×rasColor modulate, a missing color channel must pass the texture
        // through (black would kill all texture-only / colorless geometry). Present
        // CLR0/CLR1 overwrite this below.
        v.clr[0][0]=v.clr[0][1]=v.clr[0][2]=v.clr[0][3]=0xFF;
        v.clr[1][0]=v.clr[1][1]=v.clr[1][2]=v.clr[1][3]=0xFF;
    }

    // Extract each present attribute across all vertices, then scatter into the
    // interleaved native vertices. Temp buffers are stack-friendly for typical
    // packet sizes; std::vector keeps it correct for large strips.
    std::vector<float> f3((size_t)count * 3);
    std::vector<float> f2((size_t)count * 2);
    std::vector<unsigned char> c4((size_t)count * 4);

    if (((cp.vcd_lo >> 9) & 3) &&
        ngx_extract_positions(f3.data(), cp, vat, vtx, count, vstride, arr.pos, arr.pos_stride))
        for (int i = 0; i < count; i++) std::memcpy(verts[base + i].pos, &f3[(size_t)i * 3], 12);

    if (((cp.vcd_lo >> 11) & 3) &&
        ngx_extract_normals(f3.data(), cp, vat, vtx, count, vstride, arr.nrm, arr.nrm_stride))
        for (int i = 0; i < count; i++) std::memcpy(verts[base + i].nrm, &f3[(size_t)i * 3], 12);

    for (int ch = 0; ch < 2; ch++)
        if (((cp.vcd_lo >> (13 + 2 * ch)) & 3) &&
            ngx_extract_colors(c4.data(), cp, vat, ch, vtx, count, vstride,
                               arr.clr[ch], arr.clr_stride[ch]))
            for (int i = 0; i < count; i++) std::memcpy(verts[base + i].clr[ch], &c4[(size_t)i * 4], 4);

    for (int ch = 0; ch < 8; ch++)
        if (((cp.vcd_hi >> (2 * ch)) & 3) &&
            ngx_extract_texcoords(f2.data(), cp, vat, ch, vtx, count, vstride,
                                  arr.tex[ch], arr.tex_stride[ch]))
            for (int i = 0; i < count; i++) std::memcpy(verts[base + i].tex[ch], &f2[(size_t)i * 2], 8);

    // Triangulate by GX primitive type (op & 0xF8).
    const unsigned tris0 = (unsigned)indices.size();
    switch (op & 0xF8) {
    case 0x80:   // Quads
    case 0x88:   // Quads2 (unused variant — same winding)
        for (int q = 0; q + 4 <= count; q += 4) {
            tri(indices, base, q + 0, q + 1, q + 2);
            tri(indices, base, q + 0, q + 2, q + 3);
        }
        break;
    case 0x90:   // Triangles
        for (int t = 0; t + 3 <= count; t += 3) tri(indices, base, t + 0, t + 1, t + 2);
        break;
    case 0x98:   // Triangle strip (alternating winding, GL/GX convention)
        for (int i = 2; i < count; i++) {
            if (i & 1) tri(indices, base, i - 1, i - 2, i);
            else       tri(indices, base, i - 2, i - 1, i);
        }
        break;
    case 0xA0:   // Triangle fan
        for (int i = 2; i < count; i++) tri(indices, base, 0, i - 1, i);
        break;
    default:     // 0xA8 Lines, 0xB0 LineStrip, 0xB8 Points → no triangles
        break;
    }
    return (int)((indices.size() - tris0) / 3);
}

namespace {
// Context threaded through ngx_walk_stream's primitive callback.
struct BuildCtx {
    NgxCP* cp;                       // live (walk-updated) CP state
    NgxHostResolve resolve;
    void* user;
    std::vector<NgxVertex>* verts;
    std::vector<unsigned>* indices;
    int tris;
};

void build_on_prim(const NgxPrim& pr, void* u) {
    BuildCtx* c = (BuildCtx*)u;
    const NgxCP& cp = *c->cp;
    // Resolve the present indexed arrays from the live CP base/stride. Direct
    // attributes ignore the pointer (resolve_attr handles class internally), so
    // resolving an unused base is harmless.
    NgxArrays a{};
    a.pos = c->resolve(cp.array_base[0], c->user); a.pos_stride = cp.array_stride[0];
    a.nrm = c->resolve(cp.array_base[1], c->user); a.nrm_stride = cp.array_stride[1];
    a.clr[0] = c->resolve(cp.array_base[2], c->user); a.clr_stride[0] = cp.array_stride[2];
    a.clr[1] = c->resolve(cp.array_base[3], c->user); a.clr_stride[1] = cp.array_stride[3];
    for (int i = 0; i < 8; i++) {
        a.tex[i] = c->resolve(cp.array_base[4 + i], c->user);
        a.tex_stride[i] = cp.array_stride[4 + i];
    }
    c->tris += ngx_assemble_primitive(cp, pr.op, pr.vtx, pr.count, a, *c->verts, *c->indices);
}
}  // namespace

int ngx_build_mesh(const NgxCP& cp, const unsigned char* dl, size_t dl_size,
                   NgxHostResolve resolve, void* resolve_user,
                   std::vector<NgxVertex>& verts, std::vector<unsigned>& indices) {
    NgxCP work = cp;                 // walk may load CP regs; don't mutate caller's
    BuildCtx ctx{&work, resolve, resolve_user, &verts, &indices, 0};
    if (!ngx_walk_stream(dl, dl_size, work, build_on_prim, &ctx)) return -1;
    return ctx.tris;
}

// ── Self-test ───────────────────────────────────────────────────────────────
namespace {
bool approx(float a, float b) { float d = a - b; if (d < 0) d = -d; return d <= 1e-4f * (1 + (b < 0 ? -b : b)); }

void put_bef32(unsigned char* p, float f) { unsigned u; std::memcpy(&u, &f, 4);
    p[0]=u>>24;p[1]=u>>16;p[2]=u>>8;p[3]=u; }
}  // namespace

int sb_ngx_mesh_selftest(char* outbuf, int cap) {
    int pos = 0, fails = 0, cases = 0;
    auto app = [&](const char* f, ...) { if (pos >= cap) return;
        va_list a; va_start(a, f); pos += vsnprintf(outbuf + pos, cap - pos, f, a); va_end(a); };
    auto expect = [&](const char* name, bool ok, const char* detail) {
        cases++; if (!ok) { fails++; app("FAIL %-26s %s\n", name, detail); }
    };

    // CP: position direct float XYZ + color0 direct RGBA8888. vstride = 16.
    NgxCP cp{};
    cp.vcd_lo = (1u << 9) | (1u << 13);
    cp.vat[0][0] = (1u << 0) | (4u << 1) | (5u << 14);   // PosXYZ float, Color0 RGBA8888

    // Build N vertices: pos = (i,0,0), color = (i,2i,3i,255).
    auto build = [&](unsigned char* b, int n) {
        for (int i = 0; i < n; i++) {
            unsigned char* v = b + i * 16;
            put_bef32(v, (float)i); put_bef32(v + 4, 0); put_bef32(v + 8, 0);
            v[12] = (unsigned char)i; v[13] = (unsigned char)(2 * i);
            v[14] = (unsigned char)(3 * i); v[15] = 255;
        }
    };
    NgxArrays arr{};

    // 1. Quads — 4 verts → 2 triangles, indices (0,1,2)(0,2,3).
    { unsigned char b[64]; build(b, 4);
      std::vector<NgxVertex> vs; std::vector<unsigned> ix;
      int nt = ngx_assemble_primitive(cp, 0x80, b, 4, arr, vs, ix);
      expect("quad tricount", nt == 2, "expected 2 triangles");
      expect("quad vertcount", vs.size() == 4, "expected 4 verts");
      bool iok = ix.size()==6 && ix[0]==0&&ix[1]==1&&ix[2]==2 && ix[3]==0&&ix[4]==2&&ix[5]==3;
      expect("quad indices", iok, "expected (0,1,2)(0,2,3)");
      bool vok = approx(vs[2].pos[0],2.f) && vs[1].clr[0][1]==2 && vs[3].clr[0][2]==9;
      expect("quad attrs", vok, "pos/color mismatch"); }

    // 2. Triangle strip — 5 verts → 3 triangles, alternating winding.
    { unsigned char b[80]; build(b, 5);
      std::vector<NgxVertex> vs; std::vector<unsigned> ix;
      int nt = ngx_assemble_primitive(cp, 0x98, b, 5, arr, vs, ix);
      expect("strip tricount", nt == 3, "expected 3 triangles");
      bool iok = ix.size()==9 &&
                 ix[0]==0&&ix[1]==1&&ix[2]==2 &&   // i=2 even
                 ix[3]==2&&ix[4]==1&&ix[5]==3 &&   // i=3 odd  (i-1,i-2,i)
                 ix[6]==2&&ix[7]==3&&ix[8]==4;     // i=4 even (i-2,i-1,i)
      expect("strip indices", iok, "winding mismatch"); }

    // 3. Triangle fan — 4 verts → 2 triangles (0,1,2)(0,2,3).
    { unsigned char b[64]; build(b, 4);
      std::vector<NgxVertex> vs; std::vector<unsigned> ix;
      int nt = ngx_assemble_primitive(cp, 0xA0, b, 4, arr, vs, ix);
      expect("fan tricount", nt == 2, "expected 2 triangles");
      bool iok = ix.size()==6 && ix[0]==0&&ix[1]==1&&ix[2]==2 && ix[3]==0&&ix[4]==2&&ix[5]==3;
      expect("fan indices", iok, "expected (0,1,2)(0,2,3)"); }

    // 4. Triangle list — 6 verts → 2 triangles.
    { unsigned char b[96]; build(b, 6);
      std::vector<NgxVertex> vs; std::vector<unsigned> ix;
      int nt = ngx_assemble_primitive(cp, 0x90, b, 6, arr, vs, ix);
      expect("list tricount", nt == 2, "expected 2 triangles");
      expect("list indices", ix.size()==6 && ix[5]==5, "expected 6 indices ending 5"); }

    // 5. Lines emit no triangles but still assemble vertices.
    { unsigned char b[64]; build(b, 4);
      std::vector<NgxVertex> vs; std::vector<unsigned> ix;
      int nt = ngx_assemble_primitive(cp, 0xA8, b, 4, arr, vs, ix);
      expect("lines tricount", nt == 0 && ix.empty(), "expected 0 triangles");
      expect("lines vertcount", vs.size() == 4, "expected 4 verts"); }

    // 6. Append semantics — two quads into one buffer keep a rising base.
    { unsigned char b[64]; build(b, 4);
      std::vector<NgxVertex> vs; std::vector<unsigned> ix;
      ngx_assemble_primitive(cp, 0x80, b, 4, arr, vs, ix);
      ngx_assemble_primitive(cp, 0x80, b, 4, arr, vs, ix);
      expect("append vertcount", vs.size() == 8, "expected 8 verts");
      expect("append base", ix.size()==12 && ix[6]==4 && ix[8]==6, "2nd prim base != 4"); }

    // 7. Display-list build with indexed position array + direct color.
    //    cp: Position Index16 float XYZ (array 0), Color0 Direct RGBA8888.
    {
        NgxCP dcp{};
        dcp.vcd_lo = (3u << 9) | (1u << 13);              // Pos Index16, Color0 Direct
        dcp.vat[0][0] = (1u << 0) | (4u << 1) | (5u << 14);
        dcp.array_base[0] = 0x1000; dcp.array_stride[0] = 12;   // pos array @0x1000, stride 12

        // Position array: 4 entries (x = 10,11,12,13).
        static unsigned char posbuf[48];
        for (int i = 0; i < 4; i++) {
            put_bef32(posbuf + i * 12, (float)(10 + i));
            put_bef32(posbuf + i * 12 + 4, 0); put_bef32(posbuf + i * 12 + 8, 0);
        }
        // Display list: one Quads prim, 4 verts; vert = u16 index + RGBA8 color (vstride 6).
        unsigned char dl[3 + 4 * 6];
        dl[0] = 0x80; dl[1] = 0; dl[2] = 4;               // Quads, count 4
        for (int i = 0; i < 4; i++) {
            unsigned char* v = dl + 3 + i * 6;
            v[0] = 0; v[1] = (unsigned char)i;            // index = i (be16)
            v[2] = (unsigned char)(i + 1); v[3] = 0; v[4] = 0; v[5] = 255;  // color
        }
        auto resolve = [](unsigned addr, void*) -> const unsigned char* {
            if (addr >= 0x1000 && addr < 0x1000 + sizeof(posbuf)) return posbuf + (addr - 0x1000);
            return nullptr;
        };
        std::vector<NgxVertex> vs; std::vector<unsigned> ix;
        int nt = ngx_build_mesh(dcp, dl, sizeof dl, resolve, nullptr, vs, ix);
        expect("dl build tricount", nt == 2, "expected 2 triangles");
        expect("dl build vertcount", vs.size() == 4, "expected 4 verts");
        bool pok = approx(vs[0].pos[0],10.f) && approx(vs[3].pos[0],13.f);
        expect("dl indexed pos", pok, "indexed position resolution");
        expect("dl direct color", vs[2].clr[0][0]==3, "direct color in same vertex");
        bool iok = ix.size()==6 && ix[0]==0&&ix[2]==2&&ix[5]==3;
        expect("dl indices", iok, "quad triangulation");
    }

    // 8. Malformed stream → -1 (clean failure, no crash).
    {
        NgxCP dcp{}; dcp.vcd_lo = (1u<<9); dcp.vat[0][0] = (1u<<0)|(4u<<1);
        unsigned char bad[4] = {0x7F, 0, 0, 0};           // 0x7F = unknown opcode
        std::vector<NgxVertex> vs; std::vector<unsigned> ix;
        int nt = ngx_build_mesh(dcp, bad, sizeof bad, [](unsigned,void*)->const unsigned char*{return nullptr;}, nullptr, vs, ix);
        expect("malformed -> -1", nt == -1, "expected clean -1 on bad opcode");
    }

    app("ngx_mesh_selftest: %d cases, %d failing -> %s\n", cases, fails, fails==0?"OK":"MISMATCH");
    return fails;
}
