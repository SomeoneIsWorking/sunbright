// Native-audio intake (docs/native_audio_engine.md) — audio M4: the JAI SE/seq front
// end is PC-native end to end. startSoundBasic does NOT run the guest body for handled
// ids anymore; it dispatches into the native JAS engine and hands the caller a handle
// from a NATIVE-owned pool of JAISound-shaped objects in guest memory. The guest SE
// registry is therefore never populated — the whack-a-mole class where guest readers of
// that registry (frame culls, mono checks, handle-state walks) emitted garbage stops on
// never-finishing entries is structurally unrepresentable (docs/port_roadmap.md, "THE
// DECISIVE CUT").
//
// Pool contract (RE'd from JAIEntry::initSoundParameter / JAISound, reference/sms):
//  - actor code holds a JAISound* and reads/writes fields directly; pool entries ARE
//    guest memory, so direct field access (incl. JAISound::release() = recompiled guest
//    code zeroing +0x34/*+0x34) just works.
//  - +0x8 id, +0x20 actor pos Vec*, +0x34 = address of the actor's handle variable
//    (cleared when the sound dies — clearMainSoundPPointer), +0x4 prio, +0x3C info ptr.
//  - +0x40 (vptr) stays 0: the only JAISound virtuals (setSeDistance*) belong to the
//    dead guest frame processor; a null vcall here is a loud signal, not a hazard.
//  - lifecycle: the per-frame tick (on the checkNextFrameSe override seat) polls the
//    engine (njas_handle_alive) and reclaims dead entries, clearing *+0x34.
// Streams (0xC0000000-class) stay entirely on the guest path, as before.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "../cpu_state.h"
#include "../overrides.h"
#include "../intrinsics.h"

extern "C" void njas_se_start(uint32_t id, uint32_t swbit, uint8_t prio, uint32_t posPtr);
extern "C" void njas_set_camera(uint32_t posPtr, uint32_t mtxPtr);
extern "C" void njas_set_jaibasic(uint32_t self);
extern "C" void njas_bgm_start(uint32_t id, uint8_t seqTrack, uint8_t prio, uint32_t swbit);
extern "C" void njas_bgm_yoshi_percussion(int riding);
extern "C" void njas_se_stop(uint32_t id, uint32_t fade, uint32_t posPtr);
extern "C" void njas_se_param(uint32_t id, int kind, uint8_t slot, float value, uint32_t time);
extern "C" void njas_se_category_volume(uint8_t cat, uint8_t vol);
extern "C" void njas_set_wave_scene(int wsys, int scene);
extern "C" void njas_seq_port(uint32_t id, uint8_t track, uint8_t port, uint16_t value);
extern "C" int  njas_handle_alive(uint32_t id, uint32_t posPtr);
extern "C" void njas_se_stop_category(uint8_t cat);

extern "C" void func_80301e80(CPUState& cpu);   // JAIBasic::startSoundActor
extern "C" void func_80300ce4(CPUState& cpu);   // JAIBasic::setCameraInfo
extern "C" void func_803020ac(CPUState& cpu);   // JAIBasic::startSoundBasic
extern "C" void func_80301fc4(CPUState& cpu);   // JAIBasic::startSoundDirectID
extern "C" void func_80302034(CPUState& cpu);   // JAIBasic::startSoundIndirectID
extern "C" void func_80302224(CPUState& cpu);   // JAIBasic::stopSoundHandle
extern "C" void func_8030241c(CPUState& cpu);   // JAIBasic::stopAllSe(u8)
extern "C" void func_803029a4(CPUState& cpu);   // JAIBasic::setSeCategoryVolume
extern "C" void func_803017b0(CPUState& cpu);   // JAIBasic::loadSceneWave
extern "C" void func_80310994(CPUState& cpu);   // JASystem::WaveBankMgr::loadWave
extern "C" void func_8030a57c(CPUState& cpu);   // JAISound::setVolume
extern "C" void func_8030a604(CPUState& cpu);   // JAISound::setPan
extern "C" void func_8030a68c(CPUState& cpu);   // JAISound::setPitch
extern "C" void func_8030b700(CPUState& cpu);   // JAISound::setSeInterVolume
extern "C" void func_8030ad44(CPUState& cpu);   // JAISound::setSeqInterVolume
extern "C" void func_8030b330(CPUState& cpu);   // JAISound::setSeqPortData
extern "C" void func_8030b5e0(CPUState& cpu);   // JAISound::setTrackPortData
extern "C" void func_8030ae44(CPUState& cpu);   // JAISound::setSeqInterPan
extern "C" void func_8030af44(CPUState& cpu);   // JAISound::setSeqInterPitch
extern "C" void func_8030b8c8(CPUState& cpu);   // JAISound::setSeInterPan
extern "C" void func_8030be20(CPUState& cpu);   // JAISound::setSeInterPitch
extern "C" void func_8030ba90(CPUState& cpu);   // JAISound::setSeInterFxmix
extern "C" void func_8030bc58(CPUState& cpu);   // JAISound::setSeInterDolby

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
static bool guest_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x81800000u; }

// ───────────────────────── native JAISound handle pool ─────────────────────────
// Guest-memory arena in the low-mem gap (vectors end 0x80001800, DOL text 0x80003100;
// idle OSContext 0x80001900-0x80001BC8, idle stack/spin from 0x80002F00 up — see
// dolphin_hook.cpp). 48 entries x 0x48 = 0x80001C00..0x80002980, clear of both.
static constexpr uint32_t kPoolBase   = 0x80001C00u;
static constexpr uint32_t kPoolStride = 0x48u;        // sizeof(JAISound) incl. vptr slot
static constexpr int      kPoolN      = 48;           // > engine registry (32 active)

struct PoolEnt { bool used = false; bool pinned = false; uint32_t id = 0, posPtr = 0; };

// The JAI init sound: started once at JAI init into JAIBasic+0x38 (gpMSound unk38) and
// NEVER released — processFrameWork dereferences unk38->unk1 with no null check every
// frame (guest invariant: the handle is eternal). The engine starts no instance for it
// (BARC slot 0 is empty), so liveness-based reclaim would null unk38 → guest NULL deref
// (verified crash, 2026-06-12). Its pool entry is pinned.
static constexpr uint32_t kInitSoundId = 0x80000800u;
static PoolEnt g_pool[kPoolN];
static std::mutex g_pool_mtx;   // tees can arrive on several guest (host) threads

static uint32_t pool_addr(int i) { return kPoolBase + (uint32_t)i * kPoolStride; }
static int pool_index(uint32_t h) {
    if (h < kPoolBase || h >= kPoolBase + (uint32_t)kPoolN * kPoolStride) return -1;
    if ((h - kPoolBase) % kPoolStride) return -1;
    return (int)((h - kPoolBase) / kPoolStride);
}

// g_pool_mtx held. clearSlot = clearMainSoundPPointer semantics: null the actor's
// handle variable iff it still points at this entry (release() may have detached it,
// or the actor may have reassigned the slot — never stomp foreign data).
static void pool_release_locked(int i, bool clearSlot) {
    const uint32_t h = pool_addr(i);
    if (clearSlot) {
        const uint32_t slot = sb_r32(h + 0x34);
        if (guest_ptr(slot) && sb_r32(slot) == h) sb_w32(slot, 0);
    }
    g_pool[i].used = false;
}

// g_pool_mtx held. Builds the JAIEntry::initSoundParameter field image.
static uint32_t pool_alloc_locked(uint32_t id, uint32_t posPtr, uint32_t actor,
                                  uint32_t param, uint8_t prio, uint32_t info,
                                  uint32_t slotPtr) {
    int i = 0;
    while (i < kPoolN && g_pool[i].used) ++i;
    if (i == kPoolN) {
        if (dbg()) fprintf(stderr, "[se_native] handle pool FULL, id=%08x fire-and-forget\n", id);
        return 0;                                   // guest also nulls the slot on failure
    }
    const uint32_t h = pool_addr(i);
    for (uint32_t o = 0; o < kPoolStride; o += 4) sb_w32(h + o, 0);
    // State byte = 4 ("playing"): game code polls this (the boot sequencing waits for
    // the init sound to reach 4), and processFrameWork gates checkNextFrameSe — our
    // reclaim tick's seat — on unk38->unk1 >= 4. With the guest queue/prepare phases
    // gone, a dispatched sound is immediately "playing". (State 1 left the tick gate
    // closed forever → pool exhaustion, verified 2026-06-12.)
    sb_w8(h + 0x1, 4);
    sb_w8(h + 0x2, 10);                             // initSoundParameter constant
    sb_w8(h + 0x4, prio);
    sb_w8(h + 0x5, 10);                             // distanceParameterMoveTime default
    sb_w32(h + 0x8, id);
    sb_w32(h + 0x10, param);
    if (guest_ptr(actor)) {                         // JAIActor: pos/dir/up Vec*, +0xC u32
        sb_w32(h + 0x20, sb_r32(actor));
        sb_w32(h + 0x24, sb_r32(actor + 0x4));
        sb_w32(h + 0x28, sb_r32(actor + 0x8));
        sb_w32(h + 0x18, sb_r32(actor + 0xC));
    }
    sb_w32(h + 0x34, slotPtr);
    sb_w32(h + 0x3C, info);
    g_pool[i] = {true, id == kInitSoundId, id, posPtr};
    return h;
}

// Pool membership of a handle (param tees): fetch the entry's sound id under the lock.
static bool pool_id_of(uint32_t h, uint32_t& id) {
    const int pi = pool_index(h);
    if (pi < 0) return false;
    std::lock_guard<std::mutex> lk(g_pool_mtx);
    if (!g_pool[pi].used) return false;
    id = g_pool[pi].id;
    return true;
}

// ───────────────────────────────── tees ─────────────────────────────────

SUNBRIGHT_OVERRIDE(ov_startSoundActor, 0x80301e80u) {
    if (dbg()) fprintf(stderr, "[se_native] startSoundActor id=%08x\n", cpu.gpr[4]);
    func_80301e80(cpu);
}

// JAIBasic::startSoundBasic(this r3, id r4, JAISound** r5, JAIActor* r6, param r7,
// flag r8, info r9). Audio M4: handled ids never run the guest body.
SUNBRIGHT_OVERRIDE(ov_startSoundBasic, 0x803020acu) {
    const uint32_t id = cpu.gpr[4], slot = cpu.gpr[5], actor = cpu.gpr[6];
    const uint32_t param = cpu.gpr[7], info = cpu.gpr[9];
    njas_set_jaibasic(cpu.gpr[3]);   // live JAIBasic* — category-mute config source
    if (!njas_enabled() || !handled_id(id)) { func_803020ac(cpu); return; }

    uint32_t swbit = 0;
    uint8_t prio = is_seq_id(id) ? 0x40 : 0, seqTrack = 0;
    if (guest_ptr(info)) {
        swbit = sb_r32(info);                       // JAISoundInfo unk0
        prio = sb_r8(info + 4);                     // JAISoundInfo unk4
        seqTrack = sb_r8(info + 5);                 // seq slot (JAIBasic::getSeqTrackNumber)
    }
    // 3D input: JAIActor (caller-stack temp) holds the PERSISTENT game-world position
    // Vec* in its first field — copy the Vec* now (the temp dies on return).
    uint32_t posPtr = 0;
    if (is_se_id(id) && guest_ptr(actor)) {
        const uint32_t v = sb_r32(actor);
        if (guest_ptr(v)) posPtr = v;
    }
    if (dbg()) fprintf(stderr, "[se_native] startSoundBasic id=%08x slot=%08x actor=%08x info=%08x\n",
                       id, slot, actor, info);

    std::lock_guard<std::mutex> lk(g_pool_mtx);
    // JAIEntry::checkSoundHandle: a slot already holding a DIFFERENT sound gets that
    // sound stopped before the new one is stored. Same identity (id + actor pos) is a
    // revive — the engine keeps the instance, the handle stays attached.
    if (guest_ptr(slot)) {
        const int pi = pool_index(sb_r32(slot));
        if (pi >= 0 && g_pool[pi].used
            && !(g_pool[pi].id == id && g_pool[pi].posPtr == posPtr)) {
            njas_se_stop(g_pool[pi].id, 0, g_pool[pi].posPtr);
            pool_release_locked(pi, false);         // slot is overwritten below
        }
    }
    if (is_se_id(id)) njas_se_start(id, swbit, prio, posPtr);
    else              njas_bgm_start(id, seqTrack, prio, swbit);
    if (guest_ptr(slot)) {
        // JAISeEntry::storeBuffer attach semantics: a revive re-attaches the EXISTING
        // instance's handle to the caller's slot (it->unk34 = sound; *sound = it).
        int attach = -1;
        for (int i = 0; i < kPoolN; i++)
            if (g_pool[i].used && g_pool[i].id == id && g_pool[i].posPtr == posPtr) { attach = i; break; }
        uint32_t h;
        if (attach >= 0) { h = pool_addr(attach); sb_w32(h + 0x34, slot); }
        else h = pool_alloc_locked(id, posPtr, actor, param, prio, info, slot);
        sb_w32(slot, h);
    }
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

// JAIBasic::stopSoundHandle(this, JAISound* r4, fade r5). Pool handles are native-only;
// fade==0 releases the handle immediately (guest releaseSeRegist → clearMainSoundPPointer),
// fade>0 keeps it until the engine reports the instance dead (frame tick reclaims).
SUNBRIGHT_OVERRIDE(ov_stopSoundHandle, 0x80302224u) {
    const uint32_t snd = cpu.gpr[4], fade = cpu.gpr[5];
    std::unique_lock<std::mutex> lk(g_pool_mtx);
    const int pi = pool_index(snd);
    if (!njas_enabled() || pi < 0 || !g_pool[pi].used) {
        lk.unlock();
        func_80302224(cpu);                         // streams / oracle / stale handle
        return;
    }
    if (dbg()) fprintf(stderr, "[se_native] stopSoundHandle id=%08x fade=%u pos=%08x\n",
                       g_pool[pi].id, fade, g_pool[pi].posPtr);
    njas_se_stop(g_pool[pi].id, fade, g_pool[pi].posPtr);
    if (fade == 0) pool_release_locked(pi, true);
}

// JAIBasic::stopAllSe(this, category r4): the guest body walks the (now always empty)
// registry — the category sweep is native.
SUNBRIGHT_OVERRIDE(ov_stopAllSe, 0x8030241cu) {
    if (!njas_enabled()) { func_8030241c(cpu); return; }
    const uint8_t cat = (uint8_t)cpu.gpr[4];
    if (dbg()) fprintf(stderr, "[se_native] stopAllSe cat=%u\n", cat);
    njas_se_stop_category(cat);
    std::lock_guard<std::mutex> lk(g_pool_mtx);
    for (int i = 0; i < kPoolN; i++)
        if (g_pool[i].used && is_se_id(g_pool[i].id) && ((g_pool[i].id >> 12) & 0xFF) == cat)
            pool_release_locked(i, true);
}

// MSoundSE::checkMonoSound (0x80017ddc) — mono-flag (swbit 0x4000) enforcement walks the
// guest registry (empty under the pool). Owned natively at dispatch (native_jas.cpp).
SUNBRIGHT_OVERRIDE(ov_checkMonoSound, 0x80017ddcu) {
    if (!njas_enabled()) { if (RecompFunc o = recomp_raw(0x80017ddcu)) o(cpu); else call_ppc(cpu, cpu.lr); return; }
    cpu.gpr[3] = 1;   // bool true, like the guest
}

// JAIBasic::checkNextFrameSe (0x80305204) — the guest per-frame SE processor does not
// run (audio M4). Its seat is now the handle pool's reclaim tick: entries whose engine
// instance has died get their actor handle variable cleared (clearMainSoundPPointer)
// and return to the pool.
SUNBRIGHT_OVERRIDE(ov_checkNextFrameSe, 0x80305204u) {
    if (!njas_enabled()) { if (RecompFunc o = recomp_raw(0x80305204u)) o(cpu); else call_ppc(cpu, cpu.lr); return; }
    std::lock_guard<std::mutex> lk(g_pool_mtx);
    for (int i = 0; i < kPoolN; i++)
        if (g_pool[i].used && !g_pool[i].pinned
            && !njas_handle_alive(g_pool[i].id, g_pool[i].posPtr))
            pool_release_locked(i, true);
}

// JAIBasic::checkSeMovePara (in processFrameWork) dereferences the init-sound handle's
// seq-parameter struct unconditionally (unk38->getSeqParameter()->unk1755) — under the
// handle pool unk38 is a pool object with no guest param structs (+0x38 = 0), giving a
// wild read at ea 0x1755 (verified crash, 2026-06-12). Its work — ticking the guest
// per-sound move params — is owned by the engine (MovePara), so it does not run.
// The other per-frame walkers in processFrameWork stay guest: they iterate registry
// lists that are simply empty now, and checkStream is the LIVE stream path.
SUNBRIGHT_OVERRIDE(ov_checkSeMovePara, 0x80305fbcu) {
    if (!njas_enabled()) { if (RecompFunc o = recomp_raw(0x80305fbcu)) o(cpu); else call_ppc(cpu, cpu.lr); }
}

// MSBgm::setStageBgmYoshiPercussion(bool) — Yoshi mount/dismount gates the stage BGM's
// percussion layer (track 15, muted at open by the synccpu-0 callback).
SUNBRIGHT_OVERRIDE(ov_setStageBgmYoshiPercussion, 0x80016548u) {
    if (dbg()) fprintf(stderr, "[se_native] setStageBgmYoshiPercussion(%u)\n", cpu.gpr[3] & 1);
    if (njas_enabled()) njas_bgm_yoshi_percussion((int)(cpu.gpr[3] & 1));
    if (RecompFunc o = recomp_raw(0x80016548u)) o(cpu); else call_ppc(cpu, cpu.lr);
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

// Outer JAISound::setVolume/setPan/setPitch (this=r3, f1=value, r4=time, r5=slot).
// Pool handles route natively for BOTH classes (the guest funnel to the inner setters
// no longer runs) and skip the guest body — the pool entry has no guest param structs.
// Non-pool handles (streams / oracle) take the guest body unchanged.
static bool outer_param(CPUState& cpu, int kind) {
    uint32_t id;
    if (!njas_enabled() || !pool_id_of(cpu.gpr[3], id)) return false;
    njas_se_param(id, kind, (uint8_t)cpu.gpr[5], (float)cpu.fpr[1].ps0, cpu.gpr[4]);
    return true;
}
SUNBRIGHT_OVERRIDE(ov_jaisound_setVolume, 0x8030a57cu) { if (!outer_param(cpu, 0)) func_8030a57c(cpu); }
SUNBRIGHT_OVERRIDE(ov_jaisound_setPan,    0x8030a604u) { if (!outer_param(cpu, 1)) func_8030a604(cpu); }
SUNBRIGHT_OVERRIDE(ov_jaisound_setPitch,  0x8030a68cu) { if (!outer_param(cpu, 2)) func_8030a68c(cpu); }

// Seq (BGM) inter setters (this=r3, slot=r4, f1=value, time=r5): MSBgmXFade fades the
// outgoing BGM through setSeqInterVolume (NOT stopSoundHandle).
static bool inner_param_r4slot(CPUState& cpu, int kind) {
    uint32_t id;
    if (!njas_enabled() || !pool_id_of(cpu.gpr[3], id)) return false;
    njas_se_param(id, kind, (uint8_t)cpu.gpr[4], (float)cpu.fpr[1].ps0, cpu.gpr[5]);
    return true;
}
SUNBRIGHT_OVERRIDE(ov_setSeqInterVolume, 0x8030ad44u) { if (!inner_param_r4slot(cpu, 0)) func_8030ad44(cpu); }
SUNBRIGHT_OVERRIDE(ov_setSeqInterPan,    0x8030ae44u) { if (!inner_param_r4slot(cpu, 1)) func_8030ae44(cpu); }
SUNBRIGHT_OVERRIDE(ov_setSeqInterPitch,  0x8030af44u) { if (!inner_param_r4slot(cpu, 2)) func_8030af44(cpu); }

// Seq port writes — the game→BMS data channel (Yoshi-drum track gate etc.): the BMS
// reads these ports to mute/scale its tracks.
SUNBRIGHT_OVERRIDE(ov_setSeqPortData, 0x8030b330u) {
    uint32_t id;
    if (njas_enabled() && pool_id_of(cpu.gpr[3], id)) {
        njas_seq_port(id, 0xFF, (uint8_t)cpu.gpr[4], (uint16_t)cpu.gpr[5]);
        return;
    }
    func_8030b330(cpu);
}
SUNBRIGHT_OVERRIDE(ov_setTrackPortData, 0x8030b5e0u) {
    uint32_t id;
    if (njas_enabled() && pool_id_of(cpu.gpr[3], id)) {
        njas_seq_port(id, (uint8_t)cpu.gpr[4], (uint8_t)cpu.gpr[5], (uint16_t)cpu.gpr[6]);
        return;
    }
    func_8030b5e0(cpu);
}

// Inner SE setters (this=r3, slot=r4, f1=value, time=r5): the funnel for BOTH the
// public API and the per-frame distance attenuation (M2.5). Same r4-slot ABI as the
// seq inter setters; kinds 3/4 = fxmix/dolby.
SUNBRIGHT_OVERRIDE(ov_setSeInterVolume, 0x8030b700u) { if (!inner_param_r4slot(cpu, 0)) func_8030b700(cpu); }
SUNBRIGHT_OVERRIDE(ov_setSeInterPan,    0x8030b8c8u) { if (!inner_param_r4slot(cpu, 1)) func_8030b8c8(cpu); }
SUNBRIGHT_OVERRIDE(ov_setSeInterPitch,  0x8030be20u) { if (!inner_param_r4slot(cpu, 2)) func_8030be20(cpu); }
SUNBRIGHT_OVERRIDE(ov_setSeInterFxmix,  0x8030ba90u) { if (!inner_param_r4slot(cpu, 3)) func_8030ba90(cpu); }
SUNBRIGHT_OVERRIDE(ov_setSeInterDolby,  0x8030bc58u) { if (!inner_param_r4slot(cpu, 4)) func_8030bc58(cpu); }
