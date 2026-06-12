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

extern "C" void njas_se_start(uint32_t id, uint32_t swbit, uint8_t prio, uint32_t posPtr);
extern "C" void njas_set_camera(uint32_t posPtr, uint32_t mtxPtr);
extern "C" void njas_bgm_start(uint32_t id);
extern "C" void njas_se_stop(uint32_t id, uint32_t fade);
extern "C" void njas_se_param(uint32_t id, int kind, uint8_t slot, float value, uint32_t time);
extern "C" void njas_se_category_volume(uint8_t cat, uint8_t vol);
extern "C" void njas_set_wave_scene(int wsys, int scene);

extern "C" void func_80301e80(CPUState& cpu);   // JAIBasic::startSoundActor
extern "C" void func_80300ce4(CPUState& cpu);   // JAIBasic::setCameraInfo
extern "C" void func_803020ac(CPUState& cpu);   // JAIBasic::startSoundBasic
extern "C" void func_80301fc4(CPUState& cpu);   // JAIBasic::startSoundDirectID
extern "C" void func_80302034(CPUState& cpu);   // JAIBasic::startSoundIndirectID
extern "C" void func_80302224(CPUState& cpu);   // JAIBasic::stopSoundHandle
extern "C" void func_803029a4(CPUState& cpu);   // JAIBasic::setSeCategoryVolume
extern "C" void func_803017b0(CPUState& cpu);   // JAIBasic::loadSceneWave
extern "C" void func_80310994(CPUState& cpu);   // JASystem::WaveBankMgr::loadWave
extern "C" void func_8030a57c(CPUState& cpu);   // JAISound::setVolume
extern "C" void func_8030a604(CPUState& cpu);   // JAISound::setPan
extern "C" void func_8030a68c(CPUState& cpu);   // JAISound::setPitch
extern "C" void func_8030b700(CPUState& cpu);   // JAISound::setSeInterVolume
extern "C" void func_8030b8c8(CPUState& cpu);   // JAISound::setSeInterPan
extern "C" void func_8030be20(CPUState& cpu);   // JAISound::setSeInterPitch

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
    const uint32_t id = cpu.gpr[4], actor = cpu.gpr[6], info = cpu.gpr[9];
    if (dbg()) fprintf(stderr, "[se_native] startSoundBasic id=%08x actor=%08x info=%08x\n",
                       id, actor, info);
    if (njas_enabled() && is_se_id(id)) {
        uint32_t swbit = 0; uint8_t prio = 0;
        if (info >= 0x80000000u && info < 0x81800000u) {
            swbit = sb_r32(info);            // JAISoundInfo unk0
            prio = sb_r8(info + 4);          // JAISoundInfo unk4
        }
        // 3D input: JAIActor (r6, caller-stack temp) holds the PERSISTENT game-world
        // position Vec* in its first field — copy the Vec* now (the temp dies on return).
        uint32_t posPtr = 0;
        if (actor >= 0x80000000u && actor < 0x81800000u) {
            const uint32_t v = sb_r32(actor);
            if (v >= 0x80000000u && v < 0x81800000u) posPtr = v;
        }
        njas_se_start(id, swbit, prio, posPtr);
    }
    if (njas_enabled() && is_seq_id(id)) njas_bgm_start(id);
    func_803020ac(cpu);
}
// Camera input for the native 3D layer: JAIBasic::setCameraInfo(this, pos Vec* r4,
// dir Vec* r5, view Mtx* r6, cam id r7). SMS runs audioCameraMax == 1.
SUNBRIGHT_OVERRIDE(ov_setCameraInfo, 0x80300ce4u) {
    if (njas_enabled() && cpu.gpr[7] == 0)
        njas_set_camera(cpu.gpr[4], cpu.gpr[6]);
    func_80300ce4(cpu);
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
// Resident wave-scene switch. NOTE: JAIBasic::loadGroupWave is VIRTUAL and SMS
// overrides it (MSound::loadGroupWave, not in the symbol map; its wScene path loads
// the .aw itself via MSLoadWave and never reaches WaveBankMgr) — so tee the two
// covering entry points instead:
//   JAIBasic::loadSceneWave(this=r3, wsys=r4, scene=r5) — all stage wave loads
//   WaveBankMgr::loadWave(wsys=r3, scene=r4)            — init stay groups + directs
SUNBRIGHT_OVERRIDE(ov_loadSceneWave, 0x803017b0u) {
    if (dbg()) fprintf(stderr, "[se_native] loadSceneWave wsys=%d scene=%d\n",
                       (int)cpu.gpr[4], (int)cpu.gpr[5]);
    if (njas_enabled()) njas_set_wave_scene((int)cpu.gpr[4], (int)cpu.gpr[5]);
    func_803017b0(cpu);
}
SUNBRIGHT_OVERRIDE(ov_wavebank_loadWave, 0x80310994u) {
    if (dbg()) fprintf(stderr, "[se_native] WaveBankMgr::loadWave wsys=%d scene=%d\n",
                       (int)cpu.gpr[3], (int)cpu.gpr[4]);
    if (njas_enabled()) njas_set_wave_scene((int)cpu.gpr[3], (int)cpu.gpr[4]);
    func_80310994(cpu);
}
// Outer JAISound::setVolume/setPan/setPitch (this=r3, f1=value, r4=time, r5=slot):
// route SEQ (BGM) ids here. SE ids are captured one level down at the setSeInter*
// setters. FALSIFIED (2026-06-12): setSeDistance* does NOT funnel through these — the
// compiler INLINED the slot write (stfsu via getSeParameter()). Distance vol/pan/pitch
// are computed by the engine's own native 3D layer instead (njas se_3d_tick: MSHandle
// curve port; camera from the setCameraInfo tee, position from JAIActor). The tees
// below carry the explicit-API param calls (vol slots 0-3 & 5-8, UI sounds, etc.).
SUNBRIGHT_OVERRIDE(ov_jaisound_setVolume, 0x8030a57cu) {
    const uint32_t id = handle_id(cpu.gpr[3]);
    if (njas_enabled() && is_seq_id(id))
        njas_se_param(id, 0, (uint8_t)cpu.gpr[5], (float)cpu.fpr[1].ps0, cpu.gpr[4]);
    func_8030a57c(cpu);
}
SUNBRIGHT_OVERRIDE(ov_jaisound_setPan, 0x8030a604u) {
    const uint32_t id = handle_id(cpu.gpr[3]);
    if (njas_enabled() && is_seq_id(id))
        njas_se_param(id, 1, (uint8_t)cpu.gpr[5], (float)cpu.fpr[1].ps0, cpu.gpr[4]);
    func_8030a604(cpu);
}
SUNBRIGHT_OVERRIDE(ov_jaisound_setPitch, 0x8030a68cu) {
    const uint32_t id = handle_id(cpu.gpr[3]);
    if (njas_enabled() && is_seq_id(id))
        njas_se_param(id, 2, (uint8_t)cpu.gpr[5], (float)cpu.fpr[1].ps0, cpu.gpr[4]);
    func_8030a68c(cpu);
}
// Inner SE setters (this=r3, slot=r4, f1=value, time=r5): the funnel for BOTH the
// public API and the per-frame distance attenuation (M2.5).
SUNBRIGHT_OVERRIDE(ov_setSeInterVolume, 0x8030b700u) {
    const uint32_t id = handle_id(cpu.gpr[3]);
    if (njas_enabled() && is_se_id(id))
        njas_se_param(id, 0, (uint8_t)cpu.gpr[4], (float)cpu.fpr[1].ps0, cpu.gpr[5]);
    func_8030b700(cpu);
}
SUNBRIGHT_OVERRIDE(ov_setSeInterPan, 0x8030b8c8u) {
    const uint32_t id = handle_id(cpu.gpr[3]);
    if (njas_enabled() && is_se_id(id))
        njas_se_param(id, 1, (uint8_t)cpu.gpr[4], (float)cpu.fpr[1].ps0, cpu.gpr[5]);
    func_8030b8c8(cpu);
}
SUNBRIGHT_OVERRIDE(ov_setSeInterPitch, 0x8030be20u) {
    const uint32_t id = handle_id(cpu.gpr[3]);
    if (njas_enabled() && is_se_id(id))
        njas_se_param(id, 2, (uint8_t)cpu.gpr[4], (float)cpu.fpr[1].ps0, cpu.gpr[5]);
    func_8030be20(cpu);
}
