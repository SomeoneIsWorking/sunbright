// Pollution-coverage INSPECTION tee (verification harness for the Sirena-goo native port).
//
// The "Manta Storm" goo is a GPU feedback coverage texture produced by
// TPollutionCounterLayer::countTexDegree (0x8019b3a0). Under ngx present Dolphin's EFB is empty, so
// the coverage never gets produced → invisible goo. Before porting that render natively we must
// confirm, on real data, WHICH draw path produces the goo and WHAT the task queues hold.
//
// This override (gated on SUNBRIGHT_DBG_POLL; zero impact when off — the original goes straight to
// Dolphin's JIT) tees countTexDegree, reads gpPollution's counter-layer state straight from guest
// RAM (object model, not Dolphin), dumps it, then runs the original so behaviour is unchanged.
//
// Counter layer = `this` (gpr[3]); gpPollution = this - 0x70 (TPollutionManager.unk70). Layout from
// reference/sms include/Map/{PollutionCount,PollutionLayer,Manager}.hpp + ResTIMG.hpp.
#include "../overrides.h"
#include "../cpu_state.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

extern u8  mem_r8(u32 ea);
extern u16 mem_r16(u32 ea);
extern u32 mem_r32(u32 ea);
extern "C" void sb_ngx_efb_store_copy(uint32_t ea, int w, int h, const uint32_t* argb);
extern "C" void sb_ngx_efb_invalidate_tex(uint32_t ea);

namespace {

bool dbg() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_DBG_POLL"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}

// TPollutionCounterLayer field offsets (PollutionCount.hpp)
constexpr u32 CL_unk8   = 0x08;   // int  layer count
constexpr u32 CL_unk14  = 0x14;   // const TPollutionLayer** layer array
constexpr u32 CL_unk1A  = 0x1A;   // u16  tex-stamp count
constexpr u32 CL_unk1C  = 0x1C;   // TPollutionTexStamp* array
constexpr u32 CL_unk22  = 0x22;   // u16  revival-tex-stamp count
constexpr u32 CL_unk28  = 0x28;   // u16  model-stamp count
constexpr u32 CL_unkD4  = 0xD4;   // u16  joint-obj-stamp task count
constexpr u32 CL_unk178 = 0x178;  // u8*  per-layer active flag

// TPollutionLayer field offsets (PollutionLayer.hpp)
constexpr u32 L_unk30 = 0x30;   // u16  pollution type
constexpr u32 L_unk54 = 0x54;   // u8*  coverage image data (= unk58 + imageDataOffset)
constexpr u32 L_unk58 = 0x58;   // ResTIMG*

// TPollutionTexStamp (0x14 bytes): unk0 type, unk4 ResTIMG*, unk8 task count, unk10 task array
constexpr u32 TS_stride = 0x14;

void dump(u32 cl) {
    u32 gp   = cl - 0x70;
    int n    = (int)mem_r32(cl + CL_unk8);
    u32 arr  = mem_r32(cl + CL_unk14);
    u32 actA = mem_r32(cl + CL_unk178);
    u32 jcnt = mem_r16(cl + CL_unkD4);
    u32 tcnt = mem_r16(cl + CL_unk1A);
    u32 rcnt = mem_r16(cl + CL_unk22);
    u32 mcnt = mem_r16(cl + CL_unk28);
    fprintf(stderr, "[poll] gpPollution=%08x counterLayer=%08x layers=%d  tasks: joint=%u tex=%u revival=%u model=%u\n",
            gp, cl, n, jcnt, tcnt, rcnt, mcnt);
    for (int i = 0; i < n && i < 16; i++) {
        u32 lay = mem_r32(arr + 4u * i);
        u8  act = lay ? mem_r8(actA + i) : 0;
        if (!lay) { fprintf(stderr, "  [%d] layer=NULL\n", i); continue; }
        u32 type = mem_r16(lay + L_unk30);
        u32 ea54 = mem_r32(lay + L_unk54);
        u32 img  = mem_r32(lay + L_unk58);
        u32 w = 0, h = 0, fmt = 0, ido = 0;
        if (img) { fmt = mem_r8(img); w = mem_r16(img + 2); h = mem_r16(img + 4); ido = mem_r32(img + 0x1C); }
        fprintf(stderr, "  [%d] act=%u type=%u img=%08x %ux%u fmt=%u ido=%x  unk54(cov)=%08x\n",
                i, act, type, img, w, h, fmt, ido, ea54);
    }
    // tex-stamp tasks (the most likely goo producer): per stamp, type / ResTIMG / live task count
    u32 ts = mem_r32(cl + CL_unk1C);
    for (u32 i = 0; ts && i < tcnt && i < 8; i++) {
        u32 e = ts + i * TS_stride;
        fprintf(stderr, "    texstamp[%u] type=%u img=%08x ntasks=%u cap=%u\n",
                i, mem_r16(e + 0), mem_r32(e + 4), mem_r32(e + 8), mem_r32(e + 0xC));
    }
}

}  // namespace

// FALSIFIABLE TEST (SUNBRIGHT_POLL_FORCE): is the Sirena goo a coverage-masked plane sampling unk54?
// If forcing full coverage into ngx's side buffer for the layer's unk54 EA makes the goo appear, the
// theory is confirmed and the remaining work is producing the REAL coverage. Temporary diagnostic.
bool force() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_POLL_FORCE"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}

void force_full_coverage(u32 cl) {
    int n = (int)mem_r32(cl + CL_unk8);
    u32 arr = mem_r32(cl + CL_unk14);
    for (int i = 0; i < n; i++) {
        u32 lay = mem_r32(arr + 4u * i);
        if (!lay) continue;
        u32 img = mem_r32(lay + L_unk58);
        u32 ea  = mem_r32(lay + L_unk54);
        if (!img || !ea) continue;
        int w = mem_r16(img + 2), h = mem_r16(img + 4);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) continue;
        static std::vector<uint32_t> buf;
        buf.assign((size_t)w * h, 0xFFFFFFFFu);
        sb_ngx_efb_store_copy(ea, w, h, buf.data());
        sb_ngx_efb_invalidate_tex(ea);
    }
}

SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_poll_inspect, 0x8019b3a0u, dbg() || force()) {
    static u64 calls = 0;
    u32 cl  = cpu.gpr[3];
    u32 idx = cpu.gpr[4];
    // Snapshot once per ~120 layer-0 calls (≈ once a couple seconds at 60 Hz, one frame's full state).
    if (dbg() && idx == 0 && (calls++ % 120) == 0)
        dump(cl);
    if (force() && idx == 0)
        force_full_coverage(cl);
    sb_run_original_around(cpu, 0x8019b3a0u, nullptr, 0);
}
