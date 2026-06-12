// PC-native fast boot: boot straight into save File 1, Delfino Plaza — no input driving,
// no save state. This is a port of the game's own boot/scene-sequencing transitions
// (TApplication::proc state machine + the file-select→gameplay handoff), enabled with
// SUNBRIGHT_FASTBOOT=1.
//
// How the real game flows (RE'd from reference/sms decomp + GMSE01 binary):
//   proc(): BOOT → NLOGO (TGCLogoDir; setup thread loads mario.arc) → DONE → MOVIE
//   (TMovieDirector, attract movie 9) → TITLE (TSelectDir over the map-15 select scene).
//   On file confirm, TCardLoad (GC2D/CardLoad.cpp PROGRESS_UNK1B/28/29) does:
//     gpApplication.mSaveFile = file;
//     readBlock(file) → TFlagManager::load(stream)   (or firstStart() for a blank file);
//     TMarDirector::setNextStage(flag(0x40000)<1 ? 0 : 1)  // 1 = Delfino Plaza
//       → gpApplication.mNextArea = {stage, scenario 0xFF}  // 0xFF: derive from save
//   and TSelectDir::direct (0x80175ec4) records setFlag(0x40003, file) and returns
//   APP_STATE_GAMEPLAY(5); proc then copies mNextArea→mCurrArea and sets up TMarDirector.
//
// The fast path keeps the state machine but ports the transitions natively:
//   • TGCLogoDir::direct (0x80295a0c) → returns DONE immediately (logo screens skipped;
//     gameLoop still joins the NLOGO setup thread, so all archive loads complete).
//   • JDrama::TDirector::direct (0x802f7d28 — TMovieDirector does not override direct)
//     → one-shot: performs the file-confirm transition above and returns GAMEPLAY.
//       The save block is read natively from the host memcard image (native_card owns the
//       CARD layer; same image), parsed per TCardSector/TFlagManager::load (offsets
//       verified against the binary at 0x80294050), written into the live TFlagManager,
//       then guest resetCard/correctFlag (or firstStart) run for the derived state
//       (shine count → flag 0x40000, etc.). Destination is forced to Delfino Plaza
//       (stage 1) regardless of save progress — that is the point of the feature; a blank
//       File 1 gets firstStart() and still lands in the plaza.
//
// GMSE01 addresses: gpApplication 0x803E9700 (mAppState +0x08, mNextArea +0x12, mSaveFile
// +0x38); TFlagManager::smInstance via r13-0x6060; field offsets from the load() disasm.
#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern int native_card_image_fd();

namespace {

constexpr u32 GPAPPLICATION   = 0x803E9700u;
constexpr u32 GCLOGO_DIRECT   = 0x80295a0cu;  // direct__10TGCLogoDirFv
constexpr u32 MOVIE_DIRECT    = 0x802b5b30u;  // TMovieDirector::direct (vtable 0x803DFA50 + 0x64;
                                              // NOTE the ctor stores two vptrs — 0x803DFA50 is the
                                              // final one, 0x803E1D50 is the TDirector base's)
constexpr u32 GSETUP_THREAD   = 0x803FCBE8u;  // gSetupThread (OSThread, Application.cpp)
constexpr u32 OS_IS_TERMINATED= 0x80348374u;  // OSIsThreadTerminated
constexpr u32 OS_JOIN_THREAD  = 0x80348d08u;  // OSJoinThread
constexpr u32 FM_RESET_CARD   = 0x80295270u;  // resetCard__12TFlagManagerFv
constexpr u32 FM_CORRECT_FLAG = 0x802939f0u;  // correctFlag__12TFlagManagerFv
constexpr u32 FM_FIRST_START  = 0x80293ba8u;  // firstStart__12TFlagManagerFv
constexpr u32 FM_SET_FLAG     = 0x80294b1cu;  // setFlag__12TFlagManagerFUll (unnamed in funcs.txt;
                                              // verified from the TSelectDir::direct call site)
// TFlagManager field offsets (verified against load() at 0x80294050)
constexpr u32 FM_CARD_BOOLS       = 0x000;  // u8[0x77]
constexpr u32 FM_CARD_INTS        = 0x078;  // s32[21]
constexpr u32 FM_LAST_SAVE_TIME   = 0x298;  // s64
constexpr u32 FM_SAVE_TIME_BACKUP = 0x2A0;  // s64, load() zeroes it
constexpr u32 FM_SAVED_BOOLS      = 0x2A8;
constexpr u32 FM_SAVED_INTS       = 0x320;
constexpr u32 FM_SAVED_SAVE_TIME  = 0x378;

constexpr u32 APP_STATE_DONE     = 4;
constexpr u32 APP_STATE_GAMEPLAY = 5;
constexpr u32 APP_STATE_MOVIE    = 6;

bool fastboot_enabled() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_FASTBOOT"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v == 1;
}
bool g_done = false;   // one-shot: after the transition fires, everything passes through

void super(CPUState& cpu, u32 addr) {
    if (RecompFunc o = recomp_raw(addr)) o(cpu);
    else call_ppc(cpu, addr);
}

// ── Host-side read of the SMS save block from the memcard image ──────────────────────────
// GC memcard filesystem: 0x2000-byte blocks; directory at block 1 (backup 2), BAT at block 3
// (backup 4), file data from block 5. The SMS file 'super_mario_sunshine' holds TCardSector
// blocks: sector 0 = options, file N = sectors 2N+1 and 2N+2 (two copies, pick the valid one
// with the higher write count). TCardSector: u32 writeCount; u8 data[0x1FF8]; u32 checkSum.
struct SaveData {
    u8  bools[0x77];
    u32 ints[21];
    u64 last_save_time;
};

u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
u64 be64(const u8* p) { return ((u64)be32(p) << 32) | be32(p + 4); }

constexpr u32 BLK = 0x2000;

bool read_block(int fd, u32 block, u8* out) {
    return pread(fd, out, BLK, (off_t)block * BLK) == (ssize_t)BLK;
}

// CalcCheckSum (CardManager.cpp): u16 sum / inverted sum over the first 0x1FFC bytes.
u32 sector_checksum(const u8* sec) {
    u32 top = 0, bottom = 0;
    for (u32 i = 0; i < 0x1FFC; i += 2) { u16 v = be16(sec + i); top += v; bottom += (u16)~v; }
    return (top << 16) | (bottom & 0xFFFF);
}

// Find the SMS file in a directory block; returns first block or 0.
u16 find_sms_file(const u8* dir) {
    for (u32 e = 0; e < 127; e++) {
        const u8* ent = dir + e * 0x40;
        if (memcmp(ent, "GMSE01", 6) != 0) continue;
        if (memcmp(ent + 0x08, "super_mario_sunshine", 20) != 0) continue;
        return be16(ent + 0x36);
    }
    return 0;
}

// Walk the BAT chain `steps` blocks from `first`. Returns 0 on a broken chain.
u16 bat_walk(const u8* bat, u16 first, u32 steps) {
    u16 b = first;
    while (steps--) {
        if (b < 5) return 0;
        u16 next = be16(bat + 0xA + (u32)(b - 5) * 2);
        if (next == 0xFFFF || next == 0) return 0;
        b = next;
    }
    return (b >= 5) ? b : 0;
}

// Read + validate one TCardSector of the SMS file; fills writeCount, returns data ptr or null.
bool read_sector(int fd, const u8* bat, u16 first, u32 index, u8* sec, u32* write_count) {
    u16 blk = bat_walk(bat, first, index);
    if (!blk || !read_block(fd, blk, sec)) return false;
    if (sector_checksum(sec) != be32(sec + 0x1FFC)) return false;
    *write_count = be32(sec);
    return true;
}

// Loads file `file_idx` (0 = File 1) from the image. Returns false → treat as blank file.
bool load_save(u32 file_idx, SaveData* out) {
    int fd = native_card_image_fd();
    if (fd < 0) return false;

    static u8 dir[BLK], bat[BLK], seca[BLK], secb[BLK];
    u16 first = 0;
    // Try main directory then backup; same for the BAT while walking.
    for (u32 d = 1; d <= 2 && !first; d++)
        if (read_block(fd, d, dir)) first = find_sms_file(dir);
    if (!first) return false;
    if (!read_block(fd, 3, bat)) return false;

    // decideUseSector, natively: of the two copies, valid + higher write count wins.
    u32 base = file_idx * 2 + 1;
    u32 wca = 0, wcb = 0;
    bool va = read_sector(fd, bat, first, base,     seca, &wca);
    bool vb = read_sector(fd, bat, first, base + 1, secb, &wcb);
    if (!va && !vb) {
        // main BAT might be stale/corrupt — retry the whole selection with the backup BAT
        if (!read_block(fd, 4, bat)) return false;
        va = read_sector(fd, bat, first, base,     seca, &wca);
        vb = read_sector(fd, bat, first, base + 1, secb, &wcb);
        if (!va && !vb) return false;
    }
    const u8* data = ((va && !vb) || (va && wca >= wcb)) ? seca + 4 : secb + 4;

    // Stream layout per TFlagManager::load: u32 magic(4); s64 saveTime; 8+4+2+2 pad;
    // 16×u32 pad; bools[0x77]; pad to 0x200; ints[21].
    if (be32(data) != 4) return false;
    out->last_save_time = be64(data + 4);
    const u8* p = data + 4 + 8 + 8 + 4 + 2 + 2 + 64;
    memcpy(out->bools, p, sizeof out->bools);
    for (u32 i = 0; i < 21; i++) out->ints[i] = be32(p + 0x200 + i * 4);
    return true;
}

// ── decideNextScenario (MarDirectorDirect.cpp), ported ───────────────────────────────────
// TMarDirector::moveStage resolves scenario 0xFF before the stage archive mounts: for
// Delfino Plaza it picks the episode from story-progress flags. Without this the 0xFF
// reaches mountStageArchive's bounds check, loadResource fails, and proc bails to the
// movie state (observed: setup-thread exit 1 → TMarDirector::direct returned DONE).
// Flag reads are TFlagManager::getFlag semantics: 0x1xxxx = mCardBools bit (offset 0),
// 0x4xxxx = mGameInts[low] (offset 0xD0).
bool fm_card_bool(u32 fm, u32 low) { return (mem_r8(fm + (low >> 3)) >> (low & 7)) & 1; }

u32 decide_next_scenario_delfino(u32 fm) {
    if (fm_card_bool(fm, 0x3AE)) return 2;
    static const u8 kEntranceShines[7] = { 0x6, 0x10, 0x1A, 0x24, 0x2E, 0x38, 0x42 };
    bool all7 = true;
    for (u8 s : kEntranceShines) all7 &= fm_card_bool(fm, s);
    if (all7) return 9;
    if (fm_card_bool(fm, 0x389)) return 8;
    if (fm_card_bool(fm, 0x386) && fm_card_bool(fm, 0x387))
        return ((s32)mem_r32(fm + 0xD0) >= 10) ? 7 : 6;   // mGameInts[0] = flag 0x40000 shines
    if (fm_card_bool(fm, 0x385)) return 5;
    if (fm_card_bool(fm, 0x384)) return 1;
    return 0;
}

// ── The ported transition ────────────────────────────────────────────────────────────────
void do_transition(CPUState& cpu) {
    const u32 file = 0;  // File 1
    u32 fm = mem_r32(cpu.gpr[13] - 0x6060);  // TFlagManager::smInstance (SDA slot, see disasm)

    SaveData sv{};
    bool have_save = load_save(file, &sv);
    if (have_save && fm) {
        // TFlagManager::load, ported: resetCard; copy card arrays + save time (and the
        // "saved" shadow copies load() makes); zero the backup time; correctFlag.
        cpu.gpr[3] = fm; super(cpu, FM_RESET_CARD);
        for (u32 i = 0; i < sizeof sv.bools; i++) {
            mem_w8(fm + FM_CARD_BOOLS + i, sv.bools[i]);
            mem_w8(fm + FM_SAVED_BOOLS + i, sv.bools[i]);
        }
        for (u32 i = 0; i < 21; i++) {
            mem_w32(fm + FM_CARD_INTS + i * 4, sv.ints[i]);
            mem_w32(fm + FM_SAVED_INTS + i * 4, sv.ints[i]);
        }
        mem_w64(fm + FM_LAST_SAVE_TIME,   sv.last_save_time);
        mem_w64(fm + FM_SAVED_SAVE_TIME,  sv.last_save_time);
        mem_w64(fm + FM_SAVE_TIME_BACKUP, 0);
        cpu.gpr[3] = fm; super(cpu, FM_CORRECT_FLAG);
    } else if (fm) {
        cpu.gpr[3] = fm; super(cpu, FM_FIRST_START);  // blank File 1: fresh game state
    }
    // TMarDirector::moveStage, stage-1 branch: resolve the episode from the save flags
    // (setNextStage stores scenario 0xFF; moveStage normally rewrites it on stage exit —
    // our path skips the title stage director, so do it here) and clear flags
    // 0x40002/0x40003 exactly as moveStage does for a fresh Delfino entry.
    u32 scenario = fm ? decide_next_scenario_delfino(fm) : 0;
    if (fm) {
        cpu.gpr[3] = fm; cpu.gpr[4] = 0x40002; cpu.gpr[5] = 0; super(cpu, FM_SET_FLAG);
        cpu.gpr[3] = fm; cpu.gpr[4] = 0x40003; cpu.gpr[5] = 0; super(cpu, FM_SET_FLAG);
    }

    // TCardLoad PROGRESS_UNK1B + setNextStage(1): File 1 active, next area = Delfino Plaza.
    mem_w8 (GPAPPLICATION + 0x38, (u8)file);      // mSaveFile
    mem_w8 (GPAPPLICATION + 0x12, 1);             // mNextArea.stage
    mem_w8 (GPAPPLICATION + 0x13, (u8)scenario);  // mNextArea.scenario (resolved episode)
    mem_w16(GPAPPLICATION + 0x14, 0);             // mNextArea.flags

    fprintf(stderr, "[fastboot] %s File 1 → Delfino Plaza (stage 1, episode %u)\n",
            have_save ? "loaded save" : "blank save (firstStart)", scenario);
}

}  // namespace

// Skip the GC-logo screens: report DONE on the first frame. gameLoop still requires the
// NLOGO setup thread (mario.arc etc.) to finish before it advances, so no load is skipped.
SUNBRIGHT_OVERRIDE(ov_fb_gclogo_direct, GCLOGO_DIRECT) {
    if (!fastboot_enabled() || g_done) { super(cpu, GCLOGO_DIRECT); return; }
    cpu.gpr[3] = APP_STATE_DONE;
}

// One-shot: in the boot attract-movie state (proc runs it with mAppState DONE, falling
// through to the MOVIE case), perform the ported file-select transition and return
// GAMEPLAY. Later movie directors (shine intros etc.) pass through untouched.
SUNBRIGHT_OVERRIDE(ov_fb_movie_direct, MOVIE_DIRECT) {
    if (fastboot_enabled() && !g_done) {
        u32 st = mem_r8(GPAPPLICATION + 0x08);
        if (st == APP_STATE_DONE || st == APP_STATE_MOVIE) {
            // TMovieDirector::setup runs the THP open on gSetupThread. Tearing the director
            // down while that thread is still inside THPPlayerOpen/Prepare crashes the THP
            // workers (observed: THPPlayerDrawCurrentFrame recursion on a dead context), so
            // mirror the real direct(): WAIT until the setup thread terminates, join it,
            // then leave — the same sequence the game uses when movie setup fails, and the
            // dtor (THPPlayerStop/Close/Quit) handles the rest on `delete mDirector`.
            cpu.gpr[3] = GSETUP_THREAD; super(cpu, OS_IS_TERMINATED);
            if (cpu.gpr[3] == 0) { cpu.gpr[3] = 0; return; }   // APP_STATE_WAIT
            cpu.gpr[3] = GSETUP_THREAD; cpu.gpr[4] = cpu.gpr[1] - 0x20;
            super(cpu, OS_JOIN_THREAD);

            g_done = true;
            do_transition(cpu);
            cpu.gpr[3] = APP_STATE_GAMEPLAY;
            return;
        }
    }
    super(cpu, MOVIE_DIRECT);
}
