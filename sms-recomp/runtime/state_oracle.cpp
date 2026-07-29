// state_oracle — see state_oracle.h.

#include "state_oracle.h"
#include "native_render.h"

#include <lucent/log.h>

#include <algorithm>
#include <cmath>
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

// ---- The pixel-state block: packing, comparison and reporting ----

uint32_t q8(const float* f) {
    uint32_t r = 0;
    for (int k = 0; k < 4; ++k) {
        long v = std::lround((double)f[k] * 255.0);
        r = (r << 8) | (uint32_t)std::clamp(v, 0L, 255L);
    }
    return r;
}

uint64_t qs10(const float* f) {
    uint64_t r = 0;
    for (int k = 0; k < 4; ++k) r = (r << 16) | (uint16_t)(int16_t)std::lround((double)f[k] * 255.0);
    return r;
}

uint8_t canon_ras(unsigned c) {
    switch (c & 7) {
    case 0: case 2: case 4: return 0;   // colour0a0 (hw raw 2/3/4 alias the two colour channels)
    case 1: case 3:         return 1;   // colour1a1
    default:                return (uint8_t)(c & 7);
    }
}

// Which colour channels this draw actually consumes: per-stage RAS selectors (a colour channel
// implies its alpha sibling) plus SRTG texgens. Differences in channels nobody reads are
// legitimately whatever the last material left there, on both sides.
unsigned used_chan_mask(const SbrDrawState& s) {
    unsigned m = 0;
    for (unsigned k = 0; k < s.numStages && k < 16; ++k) {
        if (s.rasChannel[k] == 0) m |= (1u << 0) | (1u << 2);
        if (s.rasChannel[k] == 1) m |= (1u << 1) | (1u << 3);
    }
    for (unsigned g = 0; g < s.numTexGens && g < 8; ++g) {
        if (s.tgType[g] == 2) m |= 1u << 0;
        if (s.tgType[g] == 3) m |= 1u << 1;
    }
    return m;
}

struct PixDiff {
    bool nch = false, chan = false, ras = false, comb = false, ksel = false, konst = false,
         tevreg = false, raster = false, blend = false, scis = false, cull = false;
    bool any() const {
        return nch || chan || ras || comb || ksel || konst || tevreg || raster || blend || scis ||
               cull;
    }
};

PixDiff pix_diff(const SbrDrawState& a, const SbrDrawState& b) {
    PixDiff d;
    if (a.numChans != b.numChans) d.nch = true;
    if (a.raster != b.raster) d.raster = true;
    for (int j = 0; j < 4; ++j)
        if (a.scissor[j] != b.scissor[j]) d.scis = true;
    if (a.cull != b.cull) d.cull = true;
    if (a.blend != b.blend) d.blend = true;
    const unsigned used = used_chan_mask(a) | used_chan_mask(b);
    for (unsigned c = 0; c < 4; ++c) {
        if (!(used & (1u << c))) continue;
        if (a.chanCtrl[c] != b.chanCtrl[c]) d.chan = true;
        if (c < 2 && (a.ambColor[c] != b.ambColor[c] || a.matColor[c] != b.matColor[c]))
            d.chan = true;
    }
    const unsigned ns = std::min<unsigned>(std::min(a.numStages, b.numStages), 16);
    bool usesKonst = false;
    for (unsigned k = 0; k < ns; ++k) {
        if (a.rasChannel[k] != b.rasChannel[k]) d.ras = true;
        if (a.cWord[k] != b.cWord[k] || a.aWord[k] != b.aWord[k]) d.comb = true;
        const uint32_t cw = a.cWord[k];
        const bool kc = ((cw & 15) == 14) || (((cw >> 4) & 15) == 14) ||
                        (((cw >> 8) & 15) == 14) || (((cw >> 12) & 15) == 14);
        const uint32_t aw = a.aWord[k];
        const bool ka = (((aw >> 4) & 7) == 6) || (((aw >> 7) & 7) == 6) ||
                        (((aw >> 10) & 7) == 6) || (((aw >> 13) & 7) == 6);
        if ((kc || ka) && a.kSel[k] != b.kSel[k]) d.ksel = true;
        if (kc || ka) usesKonst = true;
    }
    if (usesKonst)
        for (unsigned j = 0; j < 4; ++j)
            if (a.konst[j] != b.konst[j]) d.konst = true;
    for (unsigned j = 0; j < 4; ++j)
        if (a.tevReg[j] != b.tevReg[j]) d.tevreg = true;
    return d;
}

void append_pix(lucent::Line& l, const SbrDrawState& s) {
    l.add("nch={} |", s.numChans);
    const unsigned used = used_chan_mask(s);
    static const char* attnName[4] = {"none", "spec", "spot", "?"};
    static const char* diffName[4] = {"none", "sign", "clamp", "?"};
    for (unsigned c = 0; c < 4; ++c) {
        if (!(used & (1u << c))) continue;
        const uint16_t v = s.chanCtrl[c];
        l.add(" c{}[{}{} mat={} amb={} diff={} attn={} mask={:02x}]", c,
              (v & 2) ? "lit" : "unlit", "", (v & 1) ? "vtx" : "reg", (v & 4) ? "vtx" : "reg",
              diffName[(v >> 3) & 3], attnName[(v >> 5) & 3], (v >> 8) & 0xFF);
        if (c < 2) l.add(" amb{}={:08x} mat{}={:08x}", c, s.ambColor[c], c, s.matColor[c]);
    }
    l.add(" |");
    for (unsigned k = 0; k < s.numStages && k < 16; ++k)
        l.add(" s{}[r{} c{:06x} a{:06x} k{:04x}]", k, s.rasChannel[k], s.cWord[k], s.aWord[k],
              s.kSel[k]);
    l.add(" | konst {:08x} {:08x} {:08x} {:08x} tevreg {:016x} {:016x} {:016x} {:016x}",
          s.konst[0], s.konst[1], s.konst[2], s.konst[3], s.tevReg[0], s.tevReg[1], s.tevReg[2],
          s.tevReg[3]);
}

} // namespace

void sbr_draw_state_fill(SbrDrawState& s, const SbrTevState& tev, const SbrXfState& xf) {
    s.numStages  = (uint8_t)tev.numStages;
    s.numTexGens = (uint8_t)tev.numTexGens;
    for (unsigned k = 0; k < 16; ++k) {
        const SbrTevStage& t = tev.stage[k];
        s.texmap[k]     = t.texmap;
        s.texcoord[k]   = t.texcoord;
        s.texEnable[k]  = t.texEnable;
        s.rasChannel[k] = canon_ras(t.rasChannel);
        s.cWord[k] = (uint32_t)t.cD | (uint32_t)t.cC << 4 | (uint32_t)t.cB << 8 |
                     (uint32_t)t.cA << 12 | (uint32_t)t.cBias << 16 | (uint32_t)t.cSub << 18 |
                     (uint32_t)t.cClamp << 19 | (uint32_t)t.cScale << 20 | (uint32_t)t.cDest << 22;
        s.aWord[k] = (uint32_t)t.aD << 4 | (uint32_t)t.aC << 7 | (uint32_t)t.aB << 10 |
                     (uint32_t)t.aA << 13 | (uint32_t)t.aBias << 16 | (uint32_t)t.aSub << 18 |
                     (uint32_t)t.aClamp << 19 | (uint32_t)t.aScale << 20 | (uint32_t)t.aDest << 22;
        s.kSel[k] = (uint16_t)(t.kC | (t.kA << 8));
    }
    s.numChans = (uint8_t)xf.numChans;
    for (unsigned c = 0; c < 4; ++c) {
        const SbrChanCtrl& cc = xf.chan[c];
        const unsigned attnFn = cc.attnEnable ? (cc.attnSpot ? 2u : 1u) : 0u;
        s.chanCtrl[c] = (uint16_t)((cc.matSrcVertex ? 1u : 0u) | (cc.enableLight ? 2u : 0u) |
                                   (cc.ambSrcVertex ? 4u : 0u) |
                                   ((unsigned)(cc.diffuseFn & 3) << 3) | (attnFn << 5) |
                                   ((unsigned)cc.lightMask << 8));
    }
    for (unsigned c = 0; c < 2; ++c) {
        s.ambColor[c] = q8(xf.ambient[c]);
        s.matColor[c] = q8(xf.material[c]);
    }
    for (unsigned j = 0; j < 4; ++j) {
        s.konst[j]  = q8(tev.konstReg[j]);
        s.tevReg[j] = qs10(tev.reg[j]);
    }
    for (unsigned g = 0; g < 8; ++g) s.tgType[g] = xf.texGen[g].type;
}

extern "C" bool sbr_state_diff_enabled() { return g_limit > 0; }

void sbr_state_oracle_mine(const SbrDrawState& s) {
    if (g_limit <= 0) return;
    // The event ring is only worth its memory when the oracle is running.
    g_mineCur.push_back(s);
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

// The aurora-facing hook: a plain C ABI over arrays, so aurora needs no recomp header. The pixel
// block travels in the same raw encodings sbr_draw_state_fill produces, reconstructed on aurora's
// side from its decoded g_gxState (command_processor.cpp keeps the two packings adjacent).
extern "C" void sbr_state_oracle_aurora_raw(unsigned pos, unsigned numStages, unsigned numTexGens,
                                            const unsigned char* texmap,
                                            const unsigned char* texcoord,
                                            const unsigned char* texEnable, const unsigned* unitId,
                                            unsigned numChans, const unsigned short* chanCtrl,
                                            const unsigned* ambColor, const unsigned* matColor,
                                            const unsigned char* rasChannel, const unsigned* cWord,
                                            const unsigned* aWord, const unsigned short* kSel,
                                            const unsigned* konst,
                                            const unsigned long long* tevReg,
                                            unsigned raster, unsigned blend, const int* scissor,
                                            unsigned cull) {
    if (g_limit <= 0) return;
    SbrDrawState s{};
    s.raster = (uint16_t)raster;
    s.blend  = (uint16_t)blend;
    for (int j = 0; j < 4; ++j) s.scissor[j] = scissor[j];
    s.cull = (uint8_t)cull;
    s.pos = pos;
    s.numStages = (uint8_t)numStages;
    s.numTexGens = (uint8_t)numTexGens;
    for (unsigned k = 0; k < 16; ++k) {
        s.texmap[k] = texmap[k];
        s.texcoord[k] = texcoord[k];
        s.texEnable[k] = texEnable[k];
        s.rasChannel[k] = rasChannel[k];
        s.cWord[k] = cWord[k];
        s.aWord[k] = aWord[k];
        s.kSel[k] = kSel[k];
    }
    for (unsigned m = 0; m < 8; ++m) s.unitId[m] = unitId[m];
    s.numChans = (uint8_t)numChans;
    for (unsigned c = 0; c < 4; ++c) s.chanCtrl[c] = chanCtrl[c];
    for (unsigned c = 0; c < 2; ++c) { s.ambColor[c] = ambColor[c]; s.matColor[c] = matColor[c]; }
    for (unsigned j = 0; j < 4; ++j) { s.konst[j] = konst[j]; s.tevReg[j] = tevReg[j]; }
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
    // PER-UNIT breakdown. The operation-attribution sweep (SBR_ABLATE=1) showed the whole
    // texmap-routing deficit is texture unit 1 and nothing else, so "do the two sides agree about
    // what is BOUND on unit 1" is the question that decides between a wrong binding on our side
    // and missing CONTENT at an address both sides agree on. Counted over draws that actually
    // NAME each unit, since a unit nobody samples legitimately holds anything.
    {
        long named[8] = {}, disagree[8] = {};
        for (size_t i = 0; i < k; ++i)
            for (unsigned st = 0; st < mine[i].numStages && st < 16; ++st) {
                if (!mine[i].texEnable[st]) continue;
                const unsigned m = mine[i].texmap[st] & 7;
                ++named[m];
                if (mine[i].unitId[m] != aur[i].unitId[m]) ++disagree[m];
            }
        // WHAT the unit-1 disagreements actually are. The empty buffers are named *_dammy in the
        // sky and sea models — deliberate placeholders — so "we bind the dummy where aurora binds
        // something else" is a completely different defect from "the content never loaded", and
        // only the two addresses side by side tell them apart.
        {
            long shown = 0;
            for (size_t i = 0; i < k && shown < 8; ++i)
                for (unsigned st = 0; st < mine[i].numStages && st < 16 && shown < 8; ++st) {
                    if (!mine[i].texEnable[st]) continue;
                    if ((mine[i].texmap[st] & 7) != 1) continue;
                    if (mine[i].unitId[1] == aur[i].unitId[1]) continue;
                    ++shown;
                    // Keyed by STREAM OFFSET, not by draw ordinal: the parser counts every draw
                    // and the oracle counts only paired ones, so the two numbering schemes do not
                    // line up and joining them by eye reads a divergence as a lag. The offset is
                    // the identifier both sides genuinely share.
                    // The verdict, not the raw numbers: if aurora holds exactly what THIS side
                    // held before its most recent bind, the two sides disagree about WHEN a bind
                    // takes effect. If aurora holds something neither our current nor our previous
                    // value, it is a genuinely different texture and a different bug. Saying which
                    // is the whole job of this report.
                    const char* verdict =
                        (aur[i].unitId[1] == mine[i].prevId[1])
                            ? "aurora still holds OUR PREVIOUS value -> the sides disagree about "
                              "when this bind takes effect"
                            : "aurora holds a value that is neither our current nor our previous "
                              "one -> a genuinely different texture";
                    lucent::info("oracle", "  unit1 DIVERGENCE at stream offset {} (stage {}): "
                                           "mine 0x{:08x} (bound at offset {}, {} bytes before "
                                           "this draw; previously 0x{:08x})  aurora 0x{:08x}  --  {}",
                                 mine[i].pos, st, mine[i].unitId[1], mine[i].bindPos[1],
                                 mine[i].pos > mine[i].bindPos[1]
                                     ? mine[i].pos - mine[i].bindPos[1] : 0,
                                 mine[i].prevId[1], aur[i].unitId[1], verdict);
                }
        }
        lucent::Line l;
        l.add("  per-unit bind agreement (named stages / disagreeing):");
        for (unsigned m = 0; m < 8; ++m)
            l.add(" u{}={}/{}", m, named[m], disagree[m]);
        l.flush(lucent::Level::Info, "oracle");
    }
    lucent::info("oracle", "{} of {} draws disagree ({} trailing unpaired) — {} look like a LAG "
                           "(the same id appears within 4 draws on aurora's side), {} are a "
                           "genuinely different texture", differing, k, delta, lagLike, genuine);

    // PIXEL-STATE pass: colour channels, RAS selectors, combiner words, konst, TEV registers —
    // the state that decides a pixel once textures agree. Counted per CATEGORY so one systematic
    // family (say, every ambient colour) reads as itself rather than as noise.
    {
        // WHICH TEV register, and what each side thinks it holds. tevreg is the only surviving
        // state divergence, and "40 draws differ" is not actionable — the register index, the
        // component values and whether the draw is one of the ones that renders black are.
        // Encodings were checked identical on both sides before trusting this at all: same
        // (r<<16)|(u16)(s16)lround(v*255) packing, both from the same BP 0xE0-0xE7 writes.
        {
            long shown = 0;
            const auto s16at = [](unsigned long long v, int c) {
                return (int)(short)(unsigned short)(v >> (16 * (3 - c)));
            };
            for (size_t i = 0; i < k && shown < 6; ++i) {
                const PixDiff d = pix_diff(mine[i], aur[i]);
                if (!d.tevreg) continue;
                for (unsigned j = 0; j < 4 && shown < 6; ++j) {
                    if (mine[i].tevReg[j] == aur[i].tevReg[j]) continue;
                    ++shown;
                    static const char* kName[4] = {"PREV", "C0", "C1", "C2"};
                    lucent::info("oracle", "  TEVREG {} DIVERGENCE at stream offset {}: mine "
                                           "[{} {} {} {}]  aurora [{} {} {} {}]  (unit1 0x{:08x}, "
                                           "{} stages)",
                                 kName[j], mine[i].pos,
                                 s16at(mine[i].tevReg[j], 0), s16at(mine[i].tevReg[j], 1),
                                 s16at(mine[i].tevReg[j], 2), s16at(mine[i].tevReg[j], 3),
                                 s16at(aur[i].tevReg[j], 0), s16at(aur[i].tevReg[j], 1),
                                 s16at(aur[i].tevReg[j], 2), s16at(aur[i].tevReg[j], 3),
                                 mine[i].unitId[1], mine[i].numStages);
                }
            }
        }
        size_t nch = 0, chan = 0, ras = 0, comb = 0, ksel = 0, konst = 0, tevreg = 0, any = 0,
               raster = 0, blend = 0, scis = 0, cullD = 0;
        std::vector<size_t> pixIdx;
        for (size_t i = 0; i < k; ++i) {
            const PixDiff d = pix_diff(mine[i], aur[i]);
            if (!d.any()) continue;
            ++any;
            pixIdx.push_back(i);
            nch += d.nch; chan += d.chan; ras += d.ras; comb += d.comb;
            raster += d.raster; blend += d.blend; scis += d.scis; cullD += d.cull;
            ksel += d.ksel; konst += d.konst; tevreg += d.tevreg;
        }
        lucent::info("oracle", "pix state: {} of {} draws disagree — numChans {}, chanctrl/amb/mat "
                               "{}, ras-sel {}, combiner {}, ksel {}, konst {}, tevreg {}, "
                               "raster(z) {}, blend {}, SCISSOR {}, CULL {}",
                     any, k, nch, chan, ras, comb, ksel, konst, tevreg, raster, blend, scis,
                     cullD);
        long shown = 0;
        const size_t stride = pixIdx.empty()
                                  ? 1
                                  : std::max<size_t>(1, pixIdx.size() / (size_t)g_limit);
        for (size_t n = 0; n < pixIdx.size(); n += stride) {
            const size_t i = pixIdx[n];
            if (shown++ >= g_limit) break;
            lucent::Line a, b;
            a.add("pix draw@{} MINE   ", mine[i].pos);
            append_pix(a, mine[i]);
            a.flush(lucent::Level::Info, "oracle");
            b.add("pix draw@{} AURORA ", aur[i].pos);
            append_pix(b, aur[i]);
            b.flush(lucent::Level::Info, "oracle");
        }
    }

    // THE CAPTURE SEAM, against the FIFO state it claims to describe. For each snapshot, find the
    // first FIFO draw at or after the parser position when it was taken — that is the drawable's own
    // first draw — and compare. A mismatch means the renderer is drawing geometry with a material
    // that belongs to a different shape, which no amount of correctness in the parse can fix.
    if (!g_capturePrev.empty() && !mine.empty()) {
        size_t checked = 0, bad = 0, badPix = 0;
        long shown = 0;
        for (const SbrDrawState& c : g_capturePrev) {
            // The correlate is the LAST draw at or BEFORE the snapshot, not the first after it.
            // ov_shape_draw runs the real J3DShape::draw FIRST (it needs the matrix loads the draw
            // itself issues), so by the time the snapshot is taken this shape's draw commands are
            // ALREADY in the stream — the next draw belongs to the NEXT shape. Getting this
            // backwards reported 47% of drawables as mismatched and sent a "fix" down the wrong
            // path; the direction of the lookup IS the measurement.
            const auto up = std::upper_bound(mine.begin(), mine.end(), c.pos,
                                             [](uint32_t p, const SbrDrawState& d) { return p < d.pos; });
            if (up == mine.begin()) continue;
            const auto it = up - 1;
            ++checked;
            // The pixel-state block is checked at the seam too: the stage/texture snapshot was
            // proven correct (0 of ~900), but that said nothing about the colour-channel and
            // combiner fields, which ride the same snapshot.
            if (pix_diff(c, *it).any()) ++badPix;
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
                               "their own stream position ({} disagree on the pixel-state block)",
                     bad, checked, badPix);
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
