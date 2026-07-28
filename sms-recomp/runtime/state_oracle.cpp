// state_oracle — see state_oracle.h.

#include "state_oracle.h"

#include <lucent/log.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace {

long g_limit = [] {
    const char* e = std::getenv("SBR_STATE_DIFF");
    return e != nullptr ? std::strtol(e, nullptr, 10) : 0L;
}();

// Per-FRAME queues, not one flat list. Aurora drains the FIFO a frame or two behind this parser,
// so the Nth draw overall is not the same draw on both sides — pairing by ordinal compared
// unrelated draws and reported 98% disagreement, which is the tool lying, not a finding. Each side
// closes a frame at its own boundary; the report pairs the oldest CLOSED frame from each queue,
// and only when their draw counts match (a mismatch means the pairing is unsafe, so it is skipped
// and said out loud rather than compared anyway).
std::vector<std::vector<SbrDrawState>> g_mineFrames, g_auroraFrames;
std::vector<SbrDrawState> g_mineCur, g_auroraCur;

// Describe one draw compactly. Only the fields that can disagree, so a report line is readable
// next to its counterpart rather than two walls of numbers.
void append(lucent::Line& l, const SbrDrawState& s) {
    l.add("stages={} texgens={} |", s.numStages, s.numTexGens);
    for (unsigned i = 0; i < s.numStages && i < 16; ++i)
        l.add(" s{}:map{}{}/c{}", i, s.texmap[i], s.texEnable[i] ? "" : "(off)", s.texcoord[i]);
    l.add(" | units");
    for (unsigned m = 0; m < 4; ++m) l.add(" {}:{:06x}", m, s.unitId[m]);
}

bool same(const SbrDrawState& a, const SbrDrawState& b) {
    if (a.numStages != b.numStages || a.numTexGens != b.numTexGens) return false;
    for (unsigned i = 0; i < a.numStages && i < 16; ++i) {
        if (a.texEnable[i] != b.texEnable[i]) return false;
        // A disabled stage's map and coordinate are don't-cares: GX feeds it nothing either way,
        // so comparing them would report differences that cannot affect a pixel.
        if (!a.texEnable[i]) continue;
        if (a.texmap[i] != b.texmap[i] || a.texcoord[i] != b.texcoord[i]) return false;
    }
    // Only the units some enabled stage actually names. A unit nobody samples legitimately holds
    // whatever the last material that used it left there, on both sides.
    for (unsigned i = 0; i < a.numStages && i < 16; ++i) {
        if (!a.texEnable[i]) continue;
        const unsigned m = a.texmap[i] & 7;
        if (a.unitId[m] != b.unitId[m]) return false;
    }
    return true;
}

} // namespace

extern "C" bool sbr_state_diff_enabled() { return g_limit > 0; }

void sbr_state_oracle_mine(const SbrDrawState& s) {
    if (g_limit > 0) g_mineCur.push_back(s);
}

void sbr_state_oracle_mine_frame_end() {
    if (g_limit <= 0 || g_mineCur.empty()) return;
    g_mineFrames.push_back(std::move(g_mineCur));
    g_mineCur.clear();
    if (g_mineFrames.size() > 8) g_mineFrames.erase(g_mineFrames.begin());
}

void sbr_state_oracle_aurora(const SbrDrawState& s) {
    if (g_limit > 0) g_auroraCur.push_back(s);
}

// Aurora closes a frame when the stream position RESTARTS: it processes one contiguous buffer per
// frame, so a position that does not advance is a new buffer.
extern "C" void sbr_state_oracle_aurora_frame_end() {
    if (g_limit <= 0 || g_auroraCur.empty()) return;
    g_auroraFrames.push_back(std::move(g_auroraCur));
    g_auroraCur.clear();
    if (g_auroraFrames.size() > 8) g_auroraFrames.erase(g_auroraFrames.begin());
}

// The aurora-facing hook: a plain C ABI over arrays, so aurora needs no recomp header.
extern "C" void sbr_state_oracle_aurora_raw(unsigned numStages, unsigned numTexGens,
                                            const unsigned char* texmap,
                                            const unsigned char* texcoord,
                                            const unsigned char* texEnable, const unsigned* unitId) {
    if (g_limit <= 0) return;
    SbrDrawState s{};
    s.numStages = (uint8_t)numStages;
    s.numTexGens = (uint8_t)numTexGens;
    for (unsigned k = 0; k < 16; ++k) {
        s.texmap[k] = texmap[k];
        s.texcoord[k] = texcoord[k];
        s.texEnable[k] = texEnable[k];
    }
    for (unsigned m = 0; m < 8; ++m) s.unitId[m] = unitId[m];
    sbr_state_oracle_aurora(s);
}

void sbr_state_oracle_report() {
    if (g_limit <= 0) return;
    if (g_mineFrames.empty() || g_auroraFrames.empty()) return;
    std::vector<SbrDrawState> mine = std::move(g_mineFrames.front());
    std::vector<SbrDrawState> aur  = std::move(g_auroraFrames.front());
    g_mineFrames.erase(g_mineFrames.begin());
    g_auroraFrames.erase(g_auroraFrames.begin());
    // A small deficit on aurora's side is expected and consistent (~25 of ~29,400): its frame
    // boundary is inferred from the stream position restarting, which cannot see the very first
    // draws of a buffer. Within ONE frame the draws still align from the start, so compare the
    // common prefix and say how many were left over. A LARGE divergence would mean the frames are
    // not the same frame, so that is still refused.
    const size_t k = std::min(mine.size(), aur.size());
    const size_t delta = mine.size() > aur.size() ? mine.size() - aur.size()
                                                  : aur.size() - mine.size();
    if (k == 0 || delta > k / 20) {
        lucent::warn("oracle", "frame draw counts differ too much ({} vs {}) — refusing to pair",
                     mine.size(), aur.size());
        return;
    }
    if (k == 0) return;
    long reported = 0;
    size_t differing = 0;
    for (size_t i = 0; i < k; ++i) {
        if (same(mine[i], aur[i])) continue;
        ++differing;
        if (reported++ >= g_limit) continue;
        lucent::Line a, b;
        a.add("draw {} MINE   ", i);
        append(a, mine[i]);
        a.flush(lucent::Level::Info, "oracle");
        b.add("draw {} AURORA ", i);
        append(b, aur[i]);
        b.flush(lucent::Level::Info, "oracle");
    }
    lucent::info("oracle", "{} of {} draws in this frame disagree on texture/TEV state ({} trailing "
                           "draws unpaired)", differing, k, delta);
}
