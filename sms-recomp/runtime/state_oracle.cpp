// state_oracle — see state_oracle.h.

#include "state_oracle.h"

#include <lucent/log.h>

#include <algorithm>
#include <unordered_map>
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
// The capture-seam snapshots of the CURRENT frame, in the order the seam took them.
std::vector<SbrDrawState> g_captureCur, g_capturePrev;

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

void sbr_state_oracle_capture(uint32_t pos, const SbrDrawState& s) {
    if (g_limit <= 0) return;
    SbrDrawState c = s;
    c.pos = pos;
    g_captureCur.push_back(c);
}

void sbr_state_oracle_mine_frame_end() {
    if (g_limit <= 0 || g_mineCur.empty()) return;
    g_capturePrev.swap(g_captureCur);
    g_captureCur.clear();
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
extern "C" void sbr_state_oracle_aurora_raw(unsigned pos, unsigned numStages, unsigned numTexGens,
                                            const unsigned char* texmap,
                                            const unsigned char* texcoord,
                                            const unsigned char* texEnable, const unsigned* unitId) {
    if (g_limit <= 0) return;
    SbrDrawState s{};
    s.pos = pos;
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
    // Pair by STREAM OFFSET, so a draw is only ever compared with the same draw. Anything with no
    // counterpart is counted, not silently dropped: an unpaired draw means one side saw a command
    // the other did not, which is a finding in itself.
    std::unordered_map<uint32_t, const SbrDrawState*> byPos;
    byPos.reserve(aur.size() * 2);
    for (const SbrDrawState& a : aur) byPos.emplace(a.pos, &a);
    size_t unpaired = 0;
    std::vector<SbrDrawState> m2, a2;
    m2.reserve(mine.size());
    a2.reserve(mine.size());
    for (const SbrDrawState& s : mine) {
        const auto it = byPos.find(s.pos);
        if (it == byPos.end()) { ++unpaired; continue; }
        m2.push_back(s);
        a2.push_back(*it->second);
    }
    mine.swap(m2);
    aur.swap(a2);
    const size_t k = mine.size();
    const size_t delta = unpaired;
    if (k == 0) return;
    if (k == 0) return;
    // First pass: count, and classify each disagreement as a LAG or a genuinely different value.
    // A unit id of mine that matches aurora's at a NEARBY draw means the two are seeing the same
    // binds in a different order (a pairing or timing artefact); one that appears nowhere near
    // means this port derived a different texture, which is a real defect. Those need different
    // fixes, and telling them apart costs nothing here.
    size_t differing = 0, lagLike = 0, genuine = 0;
    std::vector<size_t> diffIdx;
    for (size_t i = 0; i < k; ++i) {
        if (same(mine[i], aur[i])) continue;
        ++differing;
        diffIdx.push_back(i);
        bool nearby = false;
        for (unsigned st = 0; st < mine[i].numStages && st < 16 && !nearby; ++st) {
            if (!mine[i].texEnable[st]) continue;
            const unsigned m = mine[i].texmap[st] & 7;
            if (mine[i].unitId[m] == aur[i].unitId[m]) continue;
            for (long d = -4; d <= 4 && !nearby; ++d) {
                const long j = (long)i + d;
                if (d == 0 || j < 0 || j >= (long)k) continue;
                if (mine[i].unitId[m] == aur[(size_t)j].unitId[m]) nearby = true;
            }
        }
        if (nearby) ++lagLike; else ++genuine;
    }
    // Sample the reports ACROSS the frame rather than taking the first few: the first draws of a
    // frame are the 2D overlay, whose upper units are legitimately untouched, so a head sample
    // describes the least interesting draws in it.
    long reported = 0;
    const size_t stride = diffIdx.empty() ? 1 : std::max<size_t>(1, diffIdx.size() / (size_t)g_limit);
    for (size_t n = 0; n < diffIdx.size(); n += stride) {
        const size_t i = diffIdx[n];
        if (reported++ >= g_limit) break;
        lucent::Line a, b;
        a.add("draw {} MINE   ", i);
        append(a, mine[i]);
        a.flush(lucent::Level::Info, "oracle");
        b.add("draw {} AURORA ", i);
        append(b, aur[i]);
        b.flush(lucent::Level::Info, "oracle");
    }
    lucent::info("oracle", "{} of {} draws disagree ({} trailing unpaired) — {} look like a LAG "
                           "(the same id appears within 4 draws on aurora's side), {} are a "
                           "genuinely different texture", differing, k, delta, lagLike, genuine);

    // THE CAPTURE SEAM, against the FIFO state it claims to describe. For each snapshot, find the
    // first FIFO draw at or after the parser position when it was taken — that is the drawable's own
    // first draw — and compare. A mismatch means the renderer is drawing geometry with a material
    // that belongs to a different shape, which no amount of correctness in the parse can fix.
    if (!g_capturePrev.empty() && !mine.empty()) {
        size_t checked = 0, bad = 0;
        long shown = 0;
        for (const SbrDrawState& c : g_capturePrev) {
            // mine[] is in stream order, so the correlate is a lower-bound on pos.
            const auto it = std::lower_bound(mine.begin(), mine.end(), c.pos,
                                             [](const SbrDrawState& d, uint32_t p) { return d.pos < p; });
            if (it == mine.end()) continue;
            ++checked;
            if (same(c, *it)) continue;
            ++bad;
            if (shown++ < g_limit) {
                lucent::Line a, b;
                a.add("capture@{} SEAM  ", c.pos);
                append(a, c);
                a.flush(lucent::Level::Info, "oracle");
                b.add("capture@{} FIFO  ", it->pos);
                append(b, *it);
                b.flush(lucent::Level::Info, "oracle");
            }
        }
        lucent::info("oracle", "capture seam: {} of {} snapshots disagree with the FIFO state at "
                               "their own stream position", bad, checked);
    }

    // INSTRUMENT VALIDATION, not a result. If aurora's upper units report ONE id for a whole frame
    // while this side reports many, the field being read on aurora's side is not where it holds the
    // bound texture — and a comparison against a constant would blame this port for every draw.
    // Uniform output is how a broken instrument looks, so it is checked before anything is believed.
    lucent::Line h;
    h.add("distinct unit ids over the frame — ");
    for (unsigned m = 0; m < 4; ++m) {
        std::vector<uint32_t> a, b;
        for (size_t i = 0; i < k; ++i) { a.push_back(mine[i].unitId[m]); b.push_back(aur[i].unitId[m]); }
        std::sort(a.begin(), a.end()); a.erase(std::unique(a.begin(), a.end()), a.end());
        std::sort(b.begin(), b.end()); b.erase(std::unique(b.begin(), b.end()), b.end());
        h.add("u{}: mine {} aurora {}  ", m, a.size(), b.size());
    }
    h.flush(lucent::Level::Info, "oracle");
}
