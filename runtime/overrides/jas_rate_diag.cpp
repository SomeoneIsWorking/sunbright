// Diagnostic (SUNBRIGHT_JAS_RATE=1): per-second call rates of the JAS audio frame cycle —
// updateDac (per-AID tick) → vframeWork (DAC mix + ring READ advance) → mixDSP (ring read) and
// the render submissions seen by the ucode. Hardware truth: every link runs at the AID rate
// (~57/s for 70-block DMA). The link whose rate collapses relative to updateDac is the stall.
#include "../overrides.h"
#include "../intrinsics.h"
#include "../dolphin_hook.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace {
std::atomic<unsigned long> g_updatedac{0}, g_vframework{0}, g_mixdsp{0},
                           g_startseq{0}, g_updateseq{0}, g_parser{0}, g_jaistart{0}, g_jaiframe{0}, g_pfw{0}, g_entried{0}, g_mainloop{0}, g_dvdld{0}, g_dvdaram{0}, g_chkread{0}, g_x421c{0};

void tickprint() {
    static time_t last = 0;
    const time_t now = time(nullptr);
    if (now != last) {
        last = now;
        fprintf(stderr, "[jasrate] updateDac=%lu vframeWork=%lu mixDSP=%lu startSeq=%lu "
                "updateSeq=%lu parser=%lu jaiStartBasic=%lu jaiFrame=%lu pfw=%lu entried=%lu mainLoop=%lu dvdFile=%lu dvdAram=%lu chkRead=%lu x421c=%lu\n",
                g_updatedac.exchange(0), g_vframework.exchange(0), g_mixdsp.exchange(0),
                g_startseq.exchange(0), g_updateseq.exchange(0), g_parser.exchange(0),
                g_jaistart.exchange(0), g_jaiframe.exchange(0), g_pfw.exchange(0), g_entried.exchange(0), g_mainloop.exchange(0),
                g_dvdld.exchange(0), g_dvdaram.exchange(0), g_chkread.exchange(0), g_x421c.exchange(0));
    }
}

void passthru(CPUState& cpu, u32 addr, std::atomic<unsigned long>& ctr) {
    ctr.fetch_add(1, std::memory_order_relaxed);
    tickprint();
    if (RecompFunc o = recomp_raw(addr)) o(cpu); else call_ppc(cpu, cpu.lr);
}

void ov_updatedac(CPUState& cpu)  { passthru(cpu, 0x80315fc0u, g_updatedac); }
void ov_startseq(CPUState& cpu)   { passthru(cpu, 0x8031c818u, g_startseq); }
void ov_updateseq(CPUState& cpu)  { passthru(cpu, 0x8031b940u, g_updateseq); }
void ov_parser(CPUState& cpu)     { passthru(cpu, 0x80321300u, g_parser); }
void ov_jaistart(CPUState& cpu)   {
    fprintf(stderr, "[jai] startSoundBasic id=%08x\n", cpu.gpr[3]);
    passthru(cpu, 0x803020acu, g_jaistart);
}
void ov_jaiframe(CPUState& cpu)   { passthru(cpu, 0x80301c1cu, g_jaiframe); }
void ov_pfw(CPUState& cpu)        { passthru(cpu, 0x80301c3cu, g_pfw); }
void ov_entried(CPUState& cpu)    { passthru(cpu, 0x80306a1cu, g_entried); }
void ov_mainloop(CPUState& cpu)   { passthru(cpu, 0x80014da8u, g_mainloop); }
void ov_dvdld(CPUState& cpu)      { fprintf(stderr, "[jas-dvd] loadFileDvdT(%08x)\n", cpu.gpr[3]);
                                    passthru(cpu, 0x80317824u, g_dvdld); }
void ov_dvdaram(CPUState& cpu)    { fprintf(stderr, "[jas-dvd] loadToAramDvdT\n");
                                    passthru(cpu, 0x803176bcu, g_dvdaram); }
void ov_chkread(CPUState& cpu)    { passthru(cpu, 0x80307facu, g_chkread); }
void ov_x421c(CPUState& cpu)      { passthru(cpu, 0x8030421cu, g_x421c); }
void trace2(CPUState& cpu, u32 addr, const char* name) {
    static int n = 0;
    const bool show = n++ < 48;
    if (show) fprintf(stderr, "[vstart] %s(r3=%08x r4=%08x)\n", name, cpu.gpr[3], cpu.gpr[4]);
    if (RecompFunc o = recomp_raw(addr)) o(cpu); else call_ppc(cpu, cpu.lr);
    if (show) fprintf(stderr, "[vstart] %s -> %08x\n", name, cpu.gpr[3]);
}
void ov_dspalloc(CPUState& cpu)   { trace2(cpu, 0x803148acu, "TDSPChannel::alloc"); }
void ov_playlog(CPUState& cpu)    { trace2(cpu, 0x80312e14u, "playLogicalChannel"); }
void ov_dspplay(CPUState& cpu)    { trace2(cpu, 0x80314704u, "TDSPChannel::play"); }
void trace_call(CPUState& cpu, u32 addr, const char* name) {
    static int n = 0;
    if (n++ < 64)
        fprintf(stderr, "[jai] %s(r3=%08x r4=%08x r5=%08x lr=%08x)\n", name, cpu.gpr[3],
                cpu.gpr[4], cpu.gpr[5], cpu.lr);
    if (RecompFunc o = recomp_raw(addr)) o(cpu); else call_ppc(cpu, cpu.lr);
    if (n <= 64) fprintf(stderr, "[jai] %s -> %08x\n", name, cpu.gpr[3]);
}
void ov_nlogo(CPUState& cpu)      { passthru(cpu, 0x80295688u, g_jaiframe); }
void ov_startactor(CPUState& cpu) { trace_call(cpu, 0x80301e80u, "startSoundActor"); }
void ov_startdir(CPUState& cpu)   { trace_call(cpu, 0x80301fc4u, "startSoundDirectID"); }
void ov_startind(CPUState& cpu)   { trace_call(cpu, 0x80302034u, "startSoundIndirectID"); }
void ov_vframework(CPUState& cpu) { passthru(cpu, 0x80315e30u, g_vframework); }
void ov_mixdsp(CPUState& cpu)     { passthru(cpu, 0x803141b4u, g_mixdsp); }

const bool reg = [] {
    if (getenv("SUNBRIGHT_JAS_RATE")) {
        register_override(0x80315fc0u, &ov_updatedac);
        register_override(0x80315e30u, &ov_vframework);
        register_override(0x803141b4u, &ov_mixdsp);
        register_override(0x8031c818u, &ov_startseq);
        register_override(0x8031b940u, &ov_updateseq);
        register_override(0x80321300u, &ov_parser);
        register_override(0x803020acu, &ov_jaistart);
        register_override(0x80301c1cu, &ov_jaiframe);
        register_override(0x80301e80u, &ov_startactor);
        register_override(0x80301c3cu, &ov_pfw);
        register_override(0x80306a1cu, &ov_entried);
        register_override(0x80014da8u, &ov_mainloop);
        register_override(0x80317824u, &ov_dvdld);
        register_override(0x803176bcu, &ov_dvdaram);
        register_override(0x80307facu, &ov_chkread);
        register_override(0x8030421cu, &ov_x421c);
        register_override(0x803148acu, &ov_dspalloc);
        register_override(0x80312e14u, &ov_playlog);
        register_override(0x80314704u, &ov_dspplay);
        register_override(0x80301fc4u, &ov_startdir);
        register_override(0x80302034u, &ov_startind);
    }
    return true;
}();
}  // namespace
