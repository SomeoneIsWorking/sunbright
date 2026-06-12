// Native-audio intake tees (docs/native_audio_engine.md M1/M2): forward the guest JAI
// API surface into the native JAS engine. The original recomp body always runs too, so
// guest-side bookkeeping (handles, lifecycle) stays intact until M4. The guest's own
// sequenced-audio path is dead under recomp (the seq-audio frontier), so there is no
// double audio.
//
// M2 surface: SE starts carry JAISoundInfo swbit/prio; stopSoundHandle, the JAISound
// volume/pan/pitch handle ops, and setSeCategoryVolume are teed by sound id (read from
// the guest JAISound at +0x8).
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "../cpu_state.h"
#include "../overrides.h"
#include "../intrinsics.h"

extern "C" void njas_se_start(uint32_t id, uint32_t swbit, uint8_t prio);
extern "C" void njas_bgm_start(uint32_t id);
extern "C" void njas_se_stop(uint32_t id, uint32_t fade);
extern "C" void njas_se_param(uint32_t id, int kind, uint8_t slot, float value, uint32_t time);
extern "C" void njas_se_category_volume(uint8_t cat, uint8_t vol);

extern "C" void func_80301e80(CPUState& cpu);   // JAIBasic::startSoundActor
extern "C" void func_803020ac(CPUState& cpu);   // JAIBasic::startSoundBasic
extern "C" void func_80301fc4(CPUState& cpu);   // JAIBasic::startSoundDirectID
extern "C" void func_80302034(CPUState& cpu);   // JAIBasic::startSoundIndirectID
extern "C" void func_80302224(CPUState& cpu);   // JAIBasic::stopSoundHandle
extern "C" void func_803029a4(CPUState& cpu);   // JAIBasic::setSeCategoryVolume
extern "C" void func_8030a57c(CPUState& cpu);   // JAISound::setVolume
extern "C" void func_8030a604(CPUState& cpu);   // JAISound::setPan
extern "C" void func_8030a68c(CPUState& cpu);   // JAISound::setPitch

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

static bool is_se_id(uint32_t id) { return (id & 0xC0000000u) == 0; }
static bool is_seq_id(uint32_t id) { return (id & 0xC0000000u) == 0x80000000u; }   // BGM/jingle class
static bool handled_id(uint32_t id) { return is_se_id(id) || is_seq_id(id); }      // streams stay guest-side

// sound id lives at JAISound+0x8 (reference/sms JAISound.hpp unk8)
static uint32_t handle_id(uint32_t handle) { return handle ? sb_r32(handle + 0x8) : 0xC0000000u; }

SUNBRIGHT_OVERRIDE(ov_startSoundActor, 0x80301e80u) {
    if (dbg()) fprintf(stderr, "[se_native] startSoundActor id=%08x\n", cpu.gpr[4]);
    func_80301e80(cpu);
}
SUNBRIGHT_OVERRIDE(ov_startSoundBasic, 0x803020acu) {
    const uint32_t id = cpu.gpr[4], info = cpu.gpr[9];
    if (dbg()) fprintf(stderr, "[se_native] startSoundBasic id=%08x info=%08x\n", id, info);
    if (njas_enabled() && is_se_id(id)) {
        uint32_t swbit = 0; uint8_t prio = 0;
        if (info >= 0x80000000u && info < 0x81800000u) {
            swbit = sb_r32(info);            // JAISoundInfo unk0
            prio = sb_r8(info + 4);          // JAISoundInfo unk4
        }
        njas_se_start(id, swbit, prio);
    }
    if (njas_enabled() && is_seq_id(id)) njas_bgm_start(id);
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
SUNBRIGHT_OVERRIDE(ov_stopSoundHandle, 0x80302224u) {
    const uint32_t id = handle_id(cpu.gpr[4]);
    if (dbg()) fprintf(stderr, "[se_native] stopSoundHandle id=%08x fade=%u\n", id, cpu.gpr[5]);
    if (njas_enabled() && handled_id(id)) njas_se_stop(id, cpu.gpr[5]);
    func_80302224(cpu);
}
SUNBRIGHT_OVERRIDE(ov_setSeCategoryVolume, 0x803029a4u) {
    if (njas_enabled()) njas_se_category_volume((uint8_t)cpu.gpr[4], (uint8_t)cpu.gpr[5]);
    func_803029a4(cpu);
}
// JAISound::setVolume/setPan/setPitch(this=r3, f1=value, r4=time, r5=slot)
SUNBRIGHT_OVERRIDE(ov_jaisound_setVolume, 0x8030a57cu) {
    const uint32_t id = handle_id(cpu.gpr[3]);
    if (njas_enabled() && handled_id(id))
        njas_se_param(id, 0, (uint8_t)cpu.gpr[5], (float)cpu.fpr[1].ps0, cpu.gpr[4]);
    func_8030a57c(cpu);
}
SUNBRIGHT_OVERRIDE(ov_jaisound_setPan, 0x8030a604u) {
    const uint32_t id = handle_id(cpu.gpr[3]);
    if (njas_enabled() && handled_id(id))
        njas_se_param(id, 1, (uint8_t)cpu.gpr[5], (float)cpu.fpr[1].ps0, cpu.gpr[4]);
    func_8030a604(cpu);
}
SUNBRIGHT_OVERRIDE(ov_jaisound_setPitch, 0x8030a68cu) {
    const uint32_t id = handle_id(cpu.gpr[3]);
    if (njas_enabled() && handled_id(id))
        njas_se_param(id, 2, (uint8_t)cpu.gpr[5], (float)cpu.fpr[1].ps0, cpu.gpr[4]);
    func_8030a68c(cpu);
}
