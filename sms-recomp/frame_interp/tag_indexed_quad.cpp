// tag_indexed_quad.cpp — interpolate SMS's two persistent indexed quad batches.
//
// Retail TDLTexQuad::draw and TDLColorTexQuad::draw bind a double-buffered XYZ-f32 position array
// and issue one display-list draw. Question marks, splash droplets and water-spray refraction
// rebuild those eye-space vertices once per 30 Hz tick while drawing with an identity position
// matrix. Pairing identity matrices cannot move them; their indexed arrays must be interpolated.

#include "../overrides/overrides.h"
#include "../runtime/sb_assert.h"
#include "populations.h"

#include <dolphin/gx/GXAuroraControl.h>
#include <intrinsics.h>

#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" void func_80224f0c(CPUState&); // TDLColorTexQuad::draw
extern "C" void func_80225408(CPUState&); // TDLTexQuad::draw
extern "C" void func_80225144(CPUState&); // TDLColorTexQuad::requestCol
extern "C" void func_80225674(CPUState&); // TDLTexQuad::request
extern "C" void func_8027fad0(CPUState&); // TModelWaterManager::garbageCollect
extern "C" void func_80223be0(CPUState&); // TQuestionManager::request
extern "C" void func_8022392c(CPUState&); // TDLTexQuad::reset

bool sbr_lerp_enabled();
void sbr_gxfifo_draw_tag(uint64_t tag);
void sbr_gxfifo_indexed_quad_keys(const uint64_t* keys, uint16_t count);
uint64_t sbr_gxfifo_pending_tag();
u8 sbr_gxfifo_pending_pop();

namespace {
constexpr uint64_t kColorKind = 0x54444c43u; // "TDLC"
constexpr uint64_t kTexKind = 0x54444c54u;   // "TDLT"
constexpr u32 kWaterRequestReturn = 0x8027e884u;
constexpr u32 kSplashRequestReturn = 0x80266eb0u;
constexpr u32 kQuestionQuadRequestReturn = 0x80223b94u;

struct BatchKeys {
    std::vector<uint64_t> values;
};

std::unordered_map<u32, BatchKeys> g_batchKeys;
std::unordered_map<u32, std::vector<uint64_t>> g_waterParticleIds;
std::unordered_map<u32, std::vector<uint64_t>> g_questionOwners;
uint64_t g_nextWaterParticleId = 1;

void record_batch_key(u32 batch, uint64_t key) {
    g_batchKeys[batch].values.push_back(key);
}

void reset_tex_quad(CPUState& cpu) {
    g_batchKeys[cpu.gpr[3]].values.clear();
    func_8022392c(cpu);
}

std::vector<uint64_t>& ensure_water_particle_ids(u32 manager, u16 count) {
    std::vector<uint64_t>& ids = g_waterParticleIds[manager];
    // garbageCollect keeps this sidecar exactly in lockstep. A smaller retail array here can only
    // mean that the manager at this guest address was reset or replaced between collections.
    if (ids.size() > count)
        ids.clear();
    while (ids.size() < count) {
        ids.push_back(0x5700000000000000ULL | g_nextWaterParticleId++);
    }
    return ids;
}

void request_tex_quad(CPUState& cpu) {
    const u32 batch = cpu.gpr[3];
    const u32 context = cpu.gpr[28];
    const u32 requestIndex = cpu.lr == kQuestionQuadRequestReturn ? cpu.gpr[29] : cpu.gpr[30];
    const bool waterRequest =
        cpu.lr == kWaterRequestReturn && context != 0 && requestIndex < MEM_R16(context + 0x12);
    const bool questionRequest = cpu.lr == kQuestionQuadRequestReturn && context != 0;
    uint64_t stableId = 0;
    if (waterRequest) {
        stableId = ensure_water_particle_ids(context, MEM_R16(context + 0x12))[requestIndex];
    } else if (questionRequest) {
        const auto found = g_questionOwners.find(context);
        SB_ASSERT(found != g_questionOwners.end() && requestIndex < found->second.size() &&
                      found->second[requestIndex] != 0,
                  "question quad %u has no owner recorded for manager 0x%08x", requestIndex,
                  context);
        stableId = found->second[requestIndex];
    }
    func_80225674(cpu);
    if ((waterRequest || questionRequest) && cpu.gpr[3] != 0)
        record_batch_key(batch, stableId);
}

uint64_t question_owner_key(const CPUState& cpu) {
    u32 owner = 0;
    switch (cpu.lr) {
    case 0x8003fb64u: // TEnemyManager::createEnemies
    case 0x800eb6a8u: // TFruitsBoat::requestShadow
    case 0x80218178u: // TLiveActor::requestShadow
    case 0x800bb740u: // TTamaNoko::requestShadow
        owner = cpu.gpr[31];
        break;
    case 0x8005bb44u: // THinokuri2::perform
    case 0x801be990u: // TCoin::perform
        owner = cpu.gpr[29];
        break;
    case 0x8026e2a8u: // TWaterGun callsite
        owner = cpu.gpr[28];
        break;
    default:
        SB_FATAL("unidentified TQuestionManager::request caller 0x%08x", cpu.lr);
    }
    SB_ASSERT(owner != 0, "TQuestionManager::request caller 0x%08x supplied a null owner", cpu.lr);
    return static_cast<uint64_t>(cpu.lr) << 32 | owner;
}

void request_question(CPUState& cpu) {
    const u32 manager = cpu.gpr[3];
    const u16 slot = MEM_R16(manager + 0x12);
    const uint64_t owner = question_owner_key(cpu);
    func_80223be0(cpu);
    if (cpu.gpr[3] == 0)
        return;
    std::vector<uint64_t>& owners = g_questionOwners[manager];
    if (owners.size() <= slot)
        owners.resize(static_cast<size_t>(slot) + 1);
    owners[slot] = owner;
}

void request_color_tex_quad(CPUState& cpu) {
    const u32 batch = cpu.gpr[3];
    const u32 splashSlot = cpu.gpr[6];
    const bool splashRequest = cpu.lr == kSplashRequestReturn && splashSlot < 64;
    func_80225144(cpu);
    if (splashRequest && cpu.gpr[3] != 0) {
        record_batch_key(batch,
                         0x5300000000000000ULL | (static_cast<uint64_t>(batch) << 8) | splashSlot);
    }
}

void garbage_collect_water(CPUState& cpu) {
    const u32 manager = cpu.gpr[3];
    const u16 oldCount = MEM_R16(manager + 0x12);
    std::vector<uint64_t>& ids = ensure_water_particle_ids(manager, oldCount);
    std::vector<uint64_t> compacted;
    compacted.reserve(oldCount);
    for (u16 i = 0; i < oldCount; ++i) {
        if (MEM_RF32(manager + 0x14 + static_cast<u32>(i) * sizeof(float)) > 0.0f)
            compacted.push_back(ids[i]);
    }

    func_8027fad0(cpu);
    const u16 newCount = MEM_R16(manager + 0x12);
    SB_ASSERT(
        newCount == compacted.size(),
        "water-particle identity compaction disagrees with retail garbageCollect: expected %zu "
        "survivors, got %u",
        compacted.size(), static_cast<unsigned>(newCount));
    ids = std::move(compacted);
}

void draw_indexed_quad(CPUState& cpu, void (*retail)(CPUState&), uint64_t kind) {
    const u32 self = cpu.gpr[3];
    // unk8 is the active quad count. A marker with no following draw would leak onto unrelated
    // geometry, so the same retail precondition gates the bracket.
    if (!sbr_lerp_enabled() || self == 0 || MEM_R16(self + 8) == 0) {
        retail(cpu);
        return;
    }

    const uint64_t previousTag = sbr_gxfifo_pending_tag();
    const u8 previousPopulation = sbr_gxfifo_pending_pop();
    const uint64_t tag = static_cast<uint64_t>(self) << 32 | kind;
    sbr_gxfifo_draw_pop(SB_POP_TDL_QUAD);
    if (const auto found = g_batchKeys.find(self);
        found != g_batchKeys.end() && !found->second.values.empty()) {
        const std::vector<uint64_t>& keys = found->second.values;
        SB_ASSERT(keys.size() == MEM_R16(self + 8),
                  "indexed quad key count %zu disagrees with retail quad count %u for batch 0x%08x",
                  keys.size(), static_cast<unsigned>(MEM_R16(self + 8)), self);
        SB_ASSERT(keys.size() <= std::numeric_limits<uint16_t>::max(),
                  "indexed quad batch 0x%08x has too many keys: %zu", self, keys.size());
        sbr_gxfifo_indexed_quad_keys(keys.data(), static_cast<uint16_t>(keys.size()));
    }
    // A reserved tag payload is an ordered one-shot control, not an identity. The immediately
    // following real tag remains the stable key seen by the draw.
    sbr_gxfifo_draw_tag(GX_AURORA_DRAW_TAG_INDEXED_DEFORM);
    sbr_gxfifo_draw_tag(tag);
    retail(cpu);
    sbr_gxfifo_draw_tag(previousTag);
    sbr_gxfifo_draw_pop(previousPopulation);
}

void ov_color(CPUState& cpu) {
    draw_indexed_quad(cpu, func_80224f0c, kColorKind);
}
void ov_tex(CPUState& cpu) {
    draw_indexed_quad(cpu, func_80225408, kTexKind);
}
} // namespace

SB_OVERRIDE(0x80224f0cu, ov_color, "TDLColorTexQuad::draw",
            "interpolate its persistent indexed XYZ-f32 position array between simulation ticks")
SB_OVERRIDE(0x80225408u, ov_tex, "TDLTexQuad::draw",
            "interpolate its persistent indexed XYZ-f32 position array between simulation ticks")
SB_OVERRIDE(0x80225674u, request_tex_quad, "TDLTexQuad::request",
            "retain stable water-particle identities for dynamic indexed-quad interpolation")
SB_OVERRIDE(0x80225144u, request_color_tex_quad, "TDLColorTexQuad::requestCol",
            "retain stable splash-slot identities for dynamic indexed-quad interpolation")
SB_OVERRIDE(0x8027fad0u, garbage_collect_water, "TModelWaterManager::garbageCollect",
            "compact native interpolation identities by the retail particle-lifetime rule")
SB_OVERRIDE(0x80223be0u, request_question, "TQuestionManager::request",
            "retain the requesting actor identity for dynamic question-marker interpolation")
SB_OVERRIDE(0x8022392cu, reset_tex_quad, "TDLTexQuad::reset",
            "begin the exact retail lifetime of one indexed-quad identity batch")
