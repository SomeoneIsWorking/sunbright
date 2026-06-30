// GX frame-stream analyzer — see gx_parse.h.
#include "gx_parse.h"
#include <cstring>

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

    // CP state persists across frames (J3D reprograms VCD/VAT per shape; the
    // very first armed frame may mis-size until the first reprogram, which the
    // ok=false verdict catches).
    CPState cp;

    OPCODE_CALLBACK(void OnCP(u8 cmd, u32 value)) {
        cp.LoadCPReg(cmd, value);
        if (cmd >= 0xA0 && cmd <= 0xAF) {
            const u32 array = cmd - 0xA0;
            if (array == 12 || array == 13)        // XF_A pos / XF_B nrm matrix arrays
                out->mtx_arrays.push_back({offset, array, value});
        }
    }
    OPCODE_CALLBACK(void OnBP(u8 cmd, u32 value)) {
        if (cmd == BPMEM_PE_TOKEN_ID || cmd == BPMEM_PE_TOKEN_INT_ID)
            out->token_offsets.push_back(offset);
        if (cmd == BPMEM_TRIGGER_EFB_COPY)
            out->copies++;
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
    OPCODE_CALLBACK(void OnPrimitiveCommand(OpcodeDecoder::Primitive, u8, u32, u16, const u8*)) {
        out->prims++;
    }
    OPCODE_CALLBACK(void OnDisplayList(u32, u32)) { out->display_lists++; }
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

bool gxp_parse_frame(const u8* p, size_t n, GxFrameInfo& out) {
    out = GxFrameInfo{};
    g_an.out = &out;
    g_an.unknown = false;
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
bool gxp_parse_frame(const u8*, size_t, GxFrameInfo& out) { out = GxFrameInfo{}; return false; }
#endif
