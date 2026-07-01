// GX frame-stream analyzer — see gx_parse.h.
#include "gx_parse.h"
#include <cstring>
#include <cstdlib>

// Host base of guest main RAM (low 24 MB), published by memory_bridge.cpp on first RAM access.
// Used to decode display-list bodies in place: a GX_CMD_CALL_DL command in the captured FIFO carries
// only the DL's guest address+size — the vertex data lives in guest RAM, not the gather pipe — so the
// per-pass vertex count (verts_pass) must follow the DL into RAM, exactly as Dolphin's GPU does.
extern u8* g_ram_base;

#ifdef HAVE_DOLPHIN_MEMMAP
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/VertexLoaderBase.h"

namespace {

class Analyzer : public OpcodeDecoder::Callback {
public:
    GxFrameInfo* out = nullptr;
    u32 offset = 0;                  // offset of the command being decoded
    bool unknown = false;
    bool recurse_dls = false;        // decode display-list bodies from guest RAM (per-pass vert count)
    int  dl_depth = 0;               // >0 while inside a recursed DL (suppress stream-offset recording)

    // Live GX pixel state for the per-draw blend/TEV value oracle (file-select overbright). Tracked
    // from BP loads; sampled into out->draws at each primitive. efb_pass increments per EFB copy.
    BlendMode blend{};               // last BPMEM_BLENDMODE
    u32       genmode = 0;           // last BPMEM_GENMODE (numtevstages live in here)
    u8        efb_pass = 0;          // EFB copies fired so far this frame
    bool      record_draws = false;  // gate the (potentially large) per-draw record
    bool      record_tev = false;    // gate the (large) per-stage TEV combiner snapshot (SUNBRIGHT_DBG_GXTEV)
    u32       color_env[16] = {};    // live BPMEM_TEV_COLOR_ENV per stage (0xC0 + 2*stage)
    u32       alpha_env[16] = {};    // live BPMEM_TEV_ALPHA_ENV per stage (0xC1 + 2*stage)
    u32       tevreg_ra[4] = {};     // live BPMEM_TEV_COLOR_RA per reg (0xE0 + 2*reg)
    u32       tevreg_bg[4] = {};     // live BPMEM_TEV_COLOR_BG per reg (0xE1 + 2*reg)

    void record_draw(u32 prims) {
        if (!record_draws || out->draws.size() >= 8192) return;
        GenMode gm; gm.hex = genmode;
        const u8 nstages = (u8)(gm.numtevstages + 1);
        out->draws.push_back({offset, efb_pass,
            (u8)blend.src_factor.Value(), (u8)blend.dst_factor.Value(),
            (u8)blend.blend_enable.Value(), (u8)blend.subtract.Value(),
            (u8)blend.logic_op_enable.Value(), (u8)blend.logic_mode.Value(),
            (u8)blend.color_update.Value(), (u8)blend.alpha_update.Value(),
            nstages, (u8)((out->proj_type == 1) ? 1 : 0), prims, (u8)(dl_depth == 0 ? 1 : 0)});
        if (record_tev) {
            GxFrameInfo::TevSnap s{}; s.nstages = nstages;
            std::memcpy(s.color_env, color_env, sizeof color_env);
            std::memcpy(s.alpha_env, alpha_env, sizeof alpha_env);
            std::memcpy(s.tevreg_ra, tevreg_ra, sizeof tevreg_ra);
            std::memcpy(s.tevreg_bg, tevreg_bg, sizeof tevreg_bg);
            out->tev_snaps.push_back(s);
        }
    }

    // CP state persists across frames (J3D reprograms VCD/VAT per shape; the
    // very first armed frame may mis-size until the first reprogram, which the
    // ok=false verdict catches).
    CPState cp;

    OPCODE_CALLBACK(void OnCP(u8 cmd, u32 value)) {
        cp.LoadCPReg(cmd, value);
        if (cmd >= 0xA0 && cmd <= 0xAF && dl_depth == 0) {   // stream-offset record: outer stream only
            const u32 array = cmd - 0xA0;
            if (array == 12 || array == 13)        // XF_A pos / XF_B nrm matrix arrays
                out->mtx_arrays.push_back({offset, array, value});
        }
    }
    OPCODE_CALLBACK(void OnBP(u8 cmd, u32 value)) {
        if (cmd == BPMEM_PE_TOKEN_ID || cmd == BPMEM_PE_TOKEN_INT_ID)
            if (dl_depth == 0) out->token_offsets.push_back(offset);   // stream-offset record: outer only
        if (cmd == BPMEM_BLENDMODE) blend.hex = value;   // live blend equation for the per-draw oracle
        if (cmd == BPMEM_GENMODE)   genmode  = value;    // live numtevstages
        // Live per-stage TEV combiner + color/konst registers (the sea-water combiner value oracle).
        if (cmd >= BPMEM_TEV_COLOR_ENV && cmd <= BPMEM_TEV_COLOR_ENV + 0x1F) {
            const u32 stage = (cmd - BPMEM_TEV_COLOR_ENV) >> 1;
            if ((cmd & 1) == 0) color_env[stage] = value;   // 0xC0 + 2*stage = ColorCombiner
            else                alpha_env[stage] = value;   // 0xC1 + 2*stage = AlphaCombiner
        }
        if (cmd >= BPMEM_TEV_COLOR_RA && cmd <= BPMEM_TEV_COLOR_RA + 7) {
            const u32 reg = (cmd - BPMEM_TEV_COLOR_RA) >> 1;
            if ((cmd & 1) == 0) tevreg_ra[reg] = value;     // 0xE0 + 2*reg = RA word
            else                tevreg_bg[reg] = value;     // 0xE1 + 2*reg = BG word
        }
        if (cmd == BPMEM_TRIGGER_EFB_COPY) {
            out->copies++;
            // Record the copy in order so a tool can diff the render-target structure (where the EFB
            // is snapshotted to a texture vs the XFB) against native's single-target composite.
            UPE_Copy pe; pe.Hex = value;
            out->efb_copies.push_back({offset, value, out->prims,
                                       (bool)pe.copy_to_xfb, (bool)pe.clear});
            if (efb_pass < 255) ++efb_pass;              // a new render-target pass begins after a copy
        }
    }
    // Big-endian word readers over the XF load payload (the gather-pipe bytes are BE).
    static u32 be_u32(const u8* d, int word) {
        const u8* p = d + word * 4;
        return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
    }
    static float be_f32(const u8* d, int word) {
        u32 v = be_u32(d, word); float f; std::memcpy(&f, &v, 4); return f;
    }
    // Capture the XF register state the GPU consumes (the Dolphin-GX parity oracle). `address` is the
    // XF target, `count` = number of 32-bit words written from `data`. A target register T (W words)
    // is present iff [T, T+W) ⊆ [address, address+count); read it at word offset (T-address).
    OPCODE_CALLBACK(void OnXF(u16 address, u8 count, const u8* data)) {
        const u32 lo = address, hi = (u32)address + count;
        auto covers = [&](u32 t, u32 w){ return lo <= t && t + w <= hi; };

        // ── Lights: XFMEM_LIGHTS 0x600..0x680, 0x10 words/light. color@+3 (packed RGBA8 BE),
        //    position@+10..12 (floats). Mark a light valid if any of its words were written.
        for (u32 li = 0; li < 8; ++li) {
            const u32 base = 0x600 + li * 0x10;
            if (hi <= base || lo >= base + 0x10) continue;       // this load touches no part of light li
            auto& L = out->lights[li];
            if (!L.valid) { L.valid = true; out->light_loads++; }
            if (covers(base + 3, 1)) {                            // colour word
                u32 c = be_u32(data, (int)(base + 3 - lo));
                L.color[0] = ((c >> 24) & 0xFF) / 255.f;
                L.color[1] = ((c >> 16) & 0xFF) / 255.f;
                L.color[2] = ((c >>  8) & 0xFF) / 255.f;
            }
            for (int k = 0; k < 3; ++k)                           // position words 10..12
                if (covers(base + 10 + k, 1)) L.pos[k] = be_f32(data, (int)(base + 10 + k - lo));
        }

        // ── Projection: XFMEM_SETPROJECTION 0x1020 = 6 matrix floats + 1 type word (0x1026).
        if (covers(0x1020, 7)) {
            for (int k = 0; k < 6; ++k) out->proj[k] = be_f32(data, (int)(0x1020 + k - lo));
            out->proj_type = (int)be_u32(data, (int)(0x1026 - lo));   // 0 = perspective, 1 = ortho
            out->have_proj = true;
        }
        // ── Viewport: XFMEM_SETVIEWPORT 0x101a, 6 floats.
        if (covers(0x101a, 6)) {
            for (int k = 0; k < 6; ++k) out->vp[k] = be_f32(data, (int)(0x101a + k - lo));
            out->have_vp = true;
        }
        // ── Channel colours: ambient 0x100a, material 0x100c (packed RGBA8 BE); chan0 ctrl 0x100e.
        if (covers(0x100a, 1)) { u32 a = be_u32(data, (int)(0x100a - lo));
            out->amb[0] = ((a >> 24) & 0xFF)/255.f; out->amb[1] = ((a >> 16) & 0xFF)/255.f; out->amb[2] = ((a >> 8) & 0xFF)/255.f; }
        if (covers(0x100c, 1)) { u32 m = be_u32(data, (int)(0x100c - lo));
            out->matc[0] = ((m >> 24) & 0xFF)/255.f; out->matc[1] = ((m >> 16) & 0xFF)/255.f;
            out->matc[2] = ((m >> 8) & 0xFF)/255.f;  out->matc[3] = (m & 0xFF)/255.f; }
        if (covers(0x100e, 1)) out->chan0_ctrl = be_u32(data, (int)(0x100e - lo));
    }
    OPCODE_CALLBACK(void OnIndexedLoad(CPArray, u32, u16, u8)) {}
    OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive prim, u8, u32, u16 num_vertices, const u8*)) {
        out->prims++;
        // Bucket by the active projection type (the cross-engine pass discriminator). proj_type is
        // updated the moment SETPROJECTION is decoded (OnXF), so it holds the pass this draw belongs to.
        const int pass = (out->proj_type == 1) ? 1 : 0;   // 1 = ortho (HUD), else perspective (scene)
        out->prims_pass[pass]++;
        out->verts_pass[pass] += num_vertices;
        if (dl_depth == 0) out->imm_verts_pass[pass] += num_vertices;   // top-level = immediate-mode
        // TRIANGLE count — the triangulation-invariant geometry metric. num_vertices counts the RAW
        // GX primitive verts (a 52-vert strip is 52), but native's parity nverts counts POST-
        // triangulation list verts (that strip → 50 tris → 150), so the raw-vs-list vert comparison is
        // confounded (the "wave overdraw" 3900 vs 1352 was exactly this). Triangles are comparable.
        {
            u32 t = 0;
            switch (prim) {
                case OpcodeDecoder::Primitive::GX_DRAW_QUADS:
                case OpcodeDecoder::Primitive::GX_DRAW_QUADS_2:      t = num_vertices / 4 * 2; break;
                case OpcodeDecoder::Primitive::GX_DRAW_TRIANGLES:    t = num_vertices / 3; break;
                case OpcodeDecoder::Primitive::GX_DRAW_TRIANGLE_STRIP:
                case OpcodeDecoder::Primitive::GX_DRAW_TRIANGLE_FAN: t = num_vertices >= 2 ? num_vertices - 2 : 0; break;
                default: t = 0; break;   // lines/points draw no triangles
            }
            out->tris_pass[pass] += t;
        }
        // Latch the ambient the GPU uses for THIS pass's draws (the first draw of the pass sets it;
        // the scene's CLOF materials never re-set ambient, so the pass's opening value is definitive).
        if (!out->amb_pass_set[pass]) {
            out->amb_pass[pass][0] = out->amb[0]; out->amb_pass[pass][1] = out->amb[1];
            out->amb_pass[pass][2] = out->amb[2]; out->amb_pass_set[pass] = true;
        }
        record_draw(num_vertices);   // live blend/TEV value oracle (gated)
    }
    OPCODE_CALLBACK(void OnDisplayList(u32 address, u32 size)) {
        out->display_lists++;
        out->dls_pass[(out->proj_type == 1) ? 1 : 0]++;
        // Follow the DL into guest RAM so its prims/verts land in the active pass bucket. Only when
        // enabled (the parity oracle) and the body is wholly inside the host-mapped low-24 MB RAM;
        // guard depth (J3D DLs don't nest, but a corrupt size must never recurse unbounded).
        if (!recurse_dls || dl_depth >= 4 || !g_ram_base || size == 0) return;
        if ((address >> 28) != 0x8 || (address & 0x0FFFFFFFu) + size > 0x01800000u) return;
        decode_dl(g_ram_base + (address & 0x01FFFFFFu), size);
    }
    // Decode a display-list body. NOINLINE breaks the force-inline recursion (OnDisplayList ->
    // RunCommand -> OnDisplayList for nested DLs); a DL-internal unknown stops only this DL, never the
    // outer frame's `ok` verdict.
    __attribute__((noinline)) void decode_dl(const u8* body, u32 size) {
        ++dl_depth;
        const bool outer_unknown = unknown;
        for (u32 p = 0; p < size;) {
            const u32 sz = OpcodeDecoder::RunCommand(body + p, size - p, *this);
            if (sz == 0 || unknown) break;    // truncated / unknown inside the DL → stop this DL only
            p += sz;
        }
        unknown = outer_unknown;
        --dl_depth;
    }
    OPCODE_CALLBACK(void OnNop(u32)) {}
    OPCODE_CALLBACK(void OnUnknown(u8 opcode, const u8*)) {
        // 0x44 (unknown-metrics) and 0x48 (invalidate vertex cache) are real
        // 1-byte commands the decoder reports via OnUnknown; the stream stays
        // in sync (RunCommand returns 1). Anything else is a framing failure.
        if (opcode != 0x44 && opcode != 0x48) unknown = true;
    }
    OPCODE_CALLBACK(void OnCommand(const u8*, u32)) {}
    OPCODE_CALLBACK(CPState& GetCPState()) { return cp; }
    OPCODE_CALLBACK(u32 GetVertexSize(u8 vat)) {
        return VertexLoaderBase::GetVertexSize(cp.vtx_desc, cp.vtx_attr[vat]);
    }
};

Analyzer g_an;   // persistent CP state; guest threads are nthr-serialized

}  // namespace

bool gxp_parse_frame(const u8* p, size_t n, GxFrameInfo& out, bool recurse_dls) {
    out = GxFrameInfo{};
    g_an.out = &out;
    g_an.unknown = false;
    g_an.recurse_dls = recurse_dls;
    g_an.dl_depth = 0;
    g_an.efb_pass = 0;
    // Record the per-draw blend/TEV oracle only when a consumer asked for it (env-gated in
    // gx_capture.cpp). The blend/genmode state persists across frames like CP state (J3D reprograms
    // it per material), so a frame that opens mid-material still sees the live equation.
    g_an.record_draws = (std::getenv("SUNBRIGHT_DBG_GXBLEND") != nullptr)
                        || (std::getenv("SUNBRIGHT_DBG_GXDRAW") != nullptr);
    g_an.record_tev   = (std::getenv("SUNBRIGHT_DBG_GXTEV") != nullptr);
    if (g_an.record_tev) g_an.record_draws = true;   // TEV snapshot pairs with the draw record (same index)
    u32 pos = 0;
    while (pos < n) {
        g_an.offset = pos;
        const u32 sz = OpcodeDecoder::RunCommand(p + pos, (u32)(n - pos), g_an);
        if (sz == 0 || g_an.unknown) break;   // truncated or unknown opcode
        pos += sz;
    }
    out.ok = (pos == n) && !g_an.unknown;
    out.total = (u32)n;
    if (!out.ok) { out.fail_offset = pos; out.fail_opcode = pos < n ? p[pos] : 0; }
    g_an.out = nullptr;
    return out.ok;
}

#else
bool gxp_parse_frame(const u8*, size_t, GxFrameInfo& out, bool) { out = GxFrameInfo{}; return false; }
#endif
