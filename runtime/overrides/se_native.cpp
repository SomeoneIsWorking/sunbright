// M1 native-audio intake (docs/native_audio_engine.md): tee SE starts into the
// native JAS engine at JAIBasic::startSoundActor (0x80301e80 — the funnel for all
// JAI sound starts; MSoundSE::startSoundSystemSE is not in the symbol map but
// lands here). SE-class ids (top bits 0) are forwarded to njas_se_start; the
// original then runs unchanged so guest-side bookkeeping (handles, lifecycle)
// stays intact. The guest's own sequenced-audio path is dead under recomp (the
// seq-audio frontier) and is slated for removal at M4 — until then it produces
// silence, so there is no double audio.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "../cpu_state.h"
#include "../overrides.h"

extern "C" void njas_se_start(uint32_t id);
extern "C" void func_80301e80(CPUState& cpu);   // JAIBasic::startSoundActor (recomp original)

extern "C" void func_803020ac(CPUState& cpu);   // JAIBasic::startSoundBasic
extern "C" void func_80301fc4(CPUState& cpu);   // JAIBasic::startSoundDirectID
extern "C" void func_80302034(CPUState& cpu);   // JAIBasic::startSoundIndirectID

static bool njas_enabled() {
    static int disabled = -1;
    if (disabled < 0) {
        const char* e = getenv("SUNBRIGHT_NO_NJAS");
        disabled = (e && *e && *e != '0') ? 1 : 0;
    }
    return !disabled;
}

static bool dbg() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_DBG_NJAS"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}

static void tee(const char* what, uint32_t id) {
    if (dbg()) fprintf(stderr, "[se_native] %s id=%08x\n", what, id);
    if (njas_enabled() && (id & 0xC0000000u) == 0)
        njas_se_start(id);
}

SUNBRIGHT_OVERRIDE(ov_startSoundActor, 0x80301e80u) {
    if (dbg()) fprintf(stderr, "[se_native] startSoundActor id=%08x\n", cpu.gpr[4]);
    func_80301e80(cpu);
}
SUNBRIGHT_OVERRIDE(ov_startSoundBasic, 0x803020acu) {
    tee("startSoundBasic", cpu.gpr[4]);
    func_803020ac(cpu);
}
SUNBRIGHT_OVERRIDE(ov_startSoundDirectID, 0x80301fc4u) {
    if (dbg()) fprintf(stderr, "[se_native] startSoundDirectID id=%08x\n", cpu.gpr[4]);
    func_80301fc4(cpu);
}
SUNBRIGHT_OVERRIDE(ov_startSoundIndirectID, 0x80302034u) {
    if (dbg()) fprintf(stderr, "[se_native] startSoundIndirectID id=%08x\n", cpu.gpr[4]);
    func_80302034(cpu);
}
