// GX frame-stream analyzer — see gx_parse.h.
#include "gx_parse.h"

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
    OPCODE_CALLBACK(void OnXF(u16, u8, const u8*)) {}
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
