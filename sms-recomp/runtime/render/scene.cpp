// scene.cpp — the interpolated scene (see scene.h for why this replaced the tick-driven approach).

#include "scene.h"
#include "gx_light.h"
#include "gx_texgen.h"
#include "gx_texture.h"
#include "tev_eval.h"

#include <lucent/log.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

struct Snapshot {
    std::vector<SbrDrawable> items;
    std::unordered_map<uint64_t, uint32_t> index; // key -> position in items
    double time = 0.0;
    bool valid = false;
};

Snapshot g_cur, g_prev, g_building;
int g_lastCount = 0, g_matched = 0;

// Tick period, learned from the observed interval between snapshots rather than assumed to be
// 1/30 — a load hitch or a 60 Hz scene would otherwise make the interpolation run at the wrong
// speed. Smoothed, because one long tick should not permanently skew it.
double g_tickPeriod = 1.0 / 30.0;

// ---------------------------------------------------------------------------------------------
// Geometry cache. Handles are 1-based so 0 keeps meaning "no geometry".

struct Area {
    uint64_t key;
    float frac; // fraction of the viewport covered by the projected bounding box
    int verts;
    float nearestW; // smallest clip w = closest approach to the camera
};
std::vector<Area> g_area;

struct Geom {
    std::vector<SbrGeomVert> verts;
    bool multislot = false;
};
std::vector<Geom> g_geom{1}; // [0] unused: handle 0 = none
std::unordered_map<uint64_t, uint32_t> g_geomIndex;
int g_multislot = 0;
int g_splitTris = 0;
long g_alphaLow = 0, g_alphaTotal = 0, g_lumBlack = 0;
double g_lumSum = 0.0;

float g_proj[16];
bool g_projValid = false;
bool g_projThisTick = false;

// The renderer's scratch vertex buffer, kept across frames so a 60 Hz render does not allocate.
std::vector<SbrVertex> g_out;

// SBR_RENDER_DEPTHVIZ=1 — shade by depth instead of per-object colour.
const bool g_skipSkinned = [] {
    const char* e = std::getenv("SBR_RENDER_SKIP_SKINNED");
    return e != nullptr && e[0] != '\0' && e[0] != '0';
}();

const bool g_depthViz = [] {
    const char* e = std::getenv("SBR_RENDER_DEPTHVIZ");
    return e != nullptr && e[0] != '\0' && e[0] != '0';
}();

// SBR_TEXGEN=0 — feed every stage the raw TEX0 coordinate instead of running the texgen. A
// DIAGNOSTIC, not a fallback: texgen and multi-texmap landed together and the pair scored WORSE
// than neither, so each half needs to be measurable on its own. Whichever one is at fault gets
// fixed; this switch is how the question is asked, not how it is answered.
const bool g_texgenOn = [] {
    const char* e = std::getenv("SBR_TEXGEN");
    return !(e != nullptr && e[0] == '0');
}();

// Report the AUTHORITATIVE GX-light evaluation used by the shipping vertex path. Keeping a second
// arithmetic implementation here made diagnostics describe a different function than rendering:
// in particular it treated GX_AF_SPEC as a distance-attenuated spotlight. SbrLightTrace carries the
// intermediate values out of gx_light.cpp so this logger cannot drift from the result it explains.
void light_channel_trace(const SbrXfState& xf, int chan, const float vpos[3], const float nrm[3],
                         const float vtxColor[4], const char* channelName) {
    SbrLightTrace trace{};
    float out[4]{};
    sbr_light_channel(xf, chan, vpos, nrm, vtxColor, out, &trace);
    if (!trace.enabled) {
        lucent::info(channelName,
                     "  chan{}: lighting DISABLED, colour = material [{:.3f} {:.3f} "
                     "{:.3f}]",
                     chan, trace.material[0], trace.material[1], trace.material[2]);
        return;
    }
    lucent::info(channelName,
                 "  chan{}: normal[{:.3f} {:.3f} {:.3f}] vpos[{:.0f} {:.0f} {:.0f}] "
                 "acc starts at ambient [{:.3f} {:.3f} {:.3f}]",
                 chan, nrm[0], nrm[1], nrm[2], vpos[0], vpos[1], vpos[2], trace.ambient[0],
                 trace.ambient[1], trace.ambient[2]);
    // The MODEL-space normal next to the view-space one. A wrong view normal is either a bad decode
    // or a bad transform, and only seeing both says which — a ground plane's model normal is
    // [0 1 0] whatever the camera is doing.
    lucent::info(channelName, "  chan{}: (model-space normal is printed by the caller)", chan);
    for (unsigned i = 0; i < trace.lights; ++i) {
        const SbrLightTrace::Per& light = trace.light[i];
        lucent::info(channelName,
                     "    light {}: dist={:.0f} ldir[{:.3f} {:.3f} {:.3f}] cosA={:.3f} "
                     "atten={:.3f} diff={:+.3f} -> k={:+.3f}  acc now [{:.3f} {:.3f} "
                     "{:.3f}]",
                     light.index, light.dist, light.direction[0], light.direction[1],
                     light.direction[2], light.cosine, light.atten, light.diffuse,
                     light.atten * light.diffuse, light.acc[0], light.acc[1], light.acc[2]);
    }
    lucent::info(channelName, "  chan{}: mat[{:.3f} {:.3f} {:.3f}] * acc = [{:.3f} {:.3f} {:.3f}]",
                 chan, trace.material[0], trace.material[1], trace.material[2], out[0], out[1],
                 out[2]);
}

void lerp_mtx(float out[12], const float a[12], const float b[12], float t) {
    // Component-wise on a model x view matrix. Correct for translation and near-correct for the
    // rotation of a small per-tick delta; a proper decompose/slerp is the upgrade if fast spins
    // ever show shear, and is deliberately deferred rather than guessed at now.
    for (int i = 0; i < 12; ++i)
        out[i] = a[i] + (b[i] - a[i]) * t;
}

} // namespace

bool sbr_scene_has_geometry(uint64_t key) {
    return g_geomIndex.count(key) != 0;
}

// Overwrite an already-interned geometry in place, or intern it if new. 2D geometry CHANGES every
// frame under a stable identity (a counter digit keeps its screen slot while its glyph changes), so
// content-keyed interning minted a fresh entry per frame — 387k of them in one run. g_geom is a
// vector and callers hold `const Geom&` into it, so that growth reallocates and dangles those
// references: undefined behaviour, and the reason enabling 2D capture collapsed the whole frame
// rather than just leaking. Updating in place keeps the entry count bounded by the number of
// distinct elements.
uint32_t sbr_scene_update_geometry(uint64_t key, const SbrGeomVert* verts, int count) {
    if (const auto it = g_geomIndex.find(key); it != g_geomIndex.end()) {
        Geom& g = g_geom[it->second];
        g.verts.assign(verts, verts + count);
        g.multislot = false;
        for (int i = 1; i < count; ++i)
            if (verts[i].slot != verts[0].slot) {
                g.multislot = true;
                break;
            }
        return it->second;
    }
    return sbr_scene_intern_geometry(key, verts, count);
}

uint32_t sbr_scene_intern_geometry(uint64_t key, const SbrGeomVert* verts, int count) {
    if (const auto it = g_geomIndex.find(key); it != g_geomIndex.end())
        return it->second;

    const uint32_t id = (uint32_t)g_geom.size();
    g_geom.push_back(Geom{});
    Geom& g = g_geom.back();
    g.verts.assign(verts, verts + count);

    // A shape whose vertices select more than one matrix slot is SKINNED. The drawable carries one
    // matrix, so such a mesh renders with the wrong transform on every vertex outside the first
    // slot. Count it instead of hiding it: the per-vertex matrix path is the next step, and this
    // number is how big it is.
    for (int i = 1; i < count; ++i)
        if (verts[i].slot != verts[0].slot) {
            g.multislot = true;
            ++g_multislot;
            break;
        }

    g_geomIndex.emplace(key, id);
    return id;
}

uint32_t sbr_scene_geometry_for_slot(uint64_t baseKey, uint32_t baseGeom, uint32_t slot) {
    if (baseGeom == 0 || baseGeom >= g_geom.size())
        return 0;
    const Geom& base = g_geom[baseGeom];
    // Unskinned fast path: every vertex is on one slot, so the element IS the slot's geometry.
    if (!base.multislot)
        return baseGeom;

    const uint64_t key = (baseKey << 8) | (uint64_t)slot | 0x8000000000000000ull;
    if (const auto it = g_geomIndex.find(key); it != g_geomIndex.end())
        return it->second;

    std::vector<SbrGeomVert> sub;
    // Whole TRIANGLES only: a triangle whose vertices span two bones cannot be drawn by a single
    // matrix. Keeping it with the slot of its first vertex is what the hardware does per-vertex
    // only approximately — flagged rather than hidden (see sbr_scene_split_triangles).
    for (size_t t = 0; t + 2 < base.verts.size(); t += 3) {
        if (base.verts[t].slot != slot && base.verts[t + 1].slot != slot &&
            base.verts[t + 2].slot != slot)
            continue;
        if (base.verts[t].slot != base.verts[t + 1].slot ||
            base.verts[t].slot != base.verts[t + 2].slot)
            ++g_splitTris;
        if (base.verts[t].slot != slot)
            continue;
        sub.push_back(base.verts[t]);
        sub.push_back(base.verts[t + 1]);
        sub.push_back(base.verts[t + 2]);
    }
    if (sub.empty())
        return 0;

    const uint32_t id = (uint32_t)g_geom.size();
    g_geom.push_back(Geom{});
    g_geom.back().verts = std::move(sub);
    g_geomIndex.emplace(key, id);
    return id;
}

int sbr_scene_split_triangles() {
    return g_splitTris;
}

void sbr_scene_report_alpha() {
    if (g_alphaTotal == 0)
        return;
    lucent::info("nrender",
                 "vertex alpha: {:.1f}% of {} textured vertices are below 0.5 "
                 "(a washed-out frame with blending on means CLR0 alpha is wrong)",
                 100.0 * (double)g_alphaLow / (double)g_alphaTotal, g_alphaTotal);
    // Vertex COLOUR too: the frame is dark, and texture x vertexColour goes black whenever the
    // vertex colour is black. Whether CLR0 really is black or is being decoded wrong is the
    // question, and the mean answers it.
    lucent::info("nrender", "vertex colour: mean luma {:.3f}, {:.1f}% are pure black",
                 g_lumSum / (double)g_alphaTotal,
                 100.0 * (double)g_lumBlack / (double)g_alphaTotal);
    g_alphaLow = g_alphaTotal = g_lumBlack = 0;
    g_lumSum = 0.0;
}

void sbr_scene_set_projection(const float m[16]) {
    // FIRST perspective of the tick only. The game sets several — the main camera, then the
    // projections screen effects rebuild for their own passes — and the last one to be set is
    // whichever effect ran most recently, not the one the scene was drawn through. The main
    // camera's is established before the scene draw, so taking the first is what matches the
    // geometry being captured.
    if (g_projThisTick)
        return;
    std::memcpy(g_proj, m, sizeof g_proj);
    static int once = 0;
    if (once < 2) {
        ++once;
        lucent::info("nrender",
                     "projection rows: [{:.4f} {:.4f} {:.4f} {:.4f}] "
                     "[{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}] "
                     "[{:.4f} {:.4f} {:.4f} {:.4f}]",
                     m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
                     m[12], m[13], m[14], m[15]);
    }
    g_projValid = true;
    g_projThisTick = true;
}
bool sbr_scene_has_projection() {
    return g_projValid;
}
const float* sbr_scene_projection() {
    return g_projValid ? g_proj : nullptr;
}

double sbr_scene_now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
int sbr_scene_multislot_count() {
    return g_multislot;
}

// How many captured drawables are 2D? The HUD (coins, lives, the water gauge) is J2D, and J2D does
// not go through J3DShape::draw — which is the only thing this port captures. If the count is zero
// while aurora shows a HUD, the HUD is not "rendered wrong", it is NOT CAPTURED, and no amount of
// shading work will produce it. Measured rather than assumed.
// Draw commands the FIFO saw during the LAST frame, captured at the top of sbr_scene_render so the
// census below compares the same frame rather than accumulating across report intervals.
long g_lastFrameDraws = 0;

void sbr_scene_report_2d() {
    size_t ortho = 0, persp = 0;
    for (const auto& d : g_cur.items) {
        // d.is2d is the getter's own verdict at capture time. The old proj[11] heuristic counted
        // FIFO-2D drawables as PERSPECTIVE (their stored matrix keeps a nonzero [11]), which made
        // the census report 0 while 20 captured 2D drawables sat in the very list it was counting
        // — an instrument miscounting the thing it exists to count.
        (d.is2d ? ortho : persp)++;
    }
    // Against the FIFO's own draw count FOR THE SAME FRAME. Taking the count at report time
    // instead accumulated ~120 frames of draws against one frame of drawables and produced a
    // ratio near 4000x that meant nothing — unlike windows as well as unlike units. Even
    // correctly windowed these are different units: one captured drawable expands to many stream
    // draw commands, so this bounds the gap rather than measuring it.
    const long fifoDraws = g_lastFrameDraws;
    long trailing = 0, longestRun = 0;
    sbr_gxfifo_take_uncaptured(&trailing, &longestRun);
    lucent::info("nrender",
                 "  projection census: {} orthographic (2D/HUD) drawables, {} "
                 "perspective, of {} captured — the FIFO saw {} draw commands in the "
                 "same window ({}x)",
                 ortho, persp, g_cur.items.size(), fifoDraws,
                 g_cur.items.empty() ? 0.0 : (double)fifoDraws / (double)g_cur.items.size());
    // TRAILING is the clean number: draws after the LAST capture have no J3D shape behind them at
    // all, so they are provably unattributed — the HUD, drawn after the 3D scene.
    //
    // The longest inter-capture RUN is deliberately NOT presented as a gap. A single shape that
    // emits thousands of draw commands produces exactly the same figure as thousands of draws with
    // no shape behind them, so it cannot separate the two — the same unit confusion that made the
    // draws-per-drawable ratio meaningless. Reported only as context.
    lucent::info("nrender",
                 "  unattributed draws: {} trail the last J3D capture (provably outside "
                 "J3D — this is the HUD). Longest inter-capture run {} — CONTEXT ONLY, "
                 "it cannot distinguish one shape emitting many draws from many draws "
                 "with no shape",
                 trailing, longestRun);
}

void sbr_scene_report_zmodes() {
    // Which depth states the scene actually uses. If this shows ONE state, per-material depth is
    // not what is occluding the plaza and the theory is wrong — cheaper to check than to assume.
    std::unordered_map<uint32_t, int> hist;
    for (const auto& d : g_cur.items) {
        const uint32_t k =
            (uint32_t)d.depth.test << 16 | (uint32_t)d.depth.func << 8 | d.depth.write;
        ++hist[k];
    }
    for (const auto& [k, n] : hist)
        lucent::info("nrender", "  zmode test={} func={} write={} -> {} drawables", (k >> 16) & 1,
                     (k >> 8) & 7, k & 1, n);

    // How many DISTINCT textures the tick's drawables reference. If this is 1, the binding is being
    // sampled at the wrong time (the FIFO is parsed once per frame, so reading it during the draws
    // returns last frame's final state for every shape) rather than per material.
    std::unordered_map<uint32_t, int> thist;
    for (const auto& d : g_cur.items)
        ++thist[d.tex[0].addr];
    lucent::info("nrender", "  {} distinct textures across {} drawables this tick", thist.size(),
                 g_cur.items.size());

    // TEXTURE-UNIT USAGE. Every stage currently samples TEXMAP0 with the single TEX0 coordinate
    // set, because that is all the vertex carries. Supporting all eight would cost 16 more floats
    // per vertex and a decode pass per set, so MEASURE how much of the scene needs it before
    // paying for it: how many drawables reference a texmap or texcoord above 0, and how many
    // declare more than one texgen.
    {
        int multiMap = 0, multiCoord = 0, multiGen = 0, alphaTested = 0;
        std::unordered_map<uint32_t, int> coordHist, genHist, srcHist, mapHist;
        int unboundStages = 0;
        for (const auto& d : g_cur.items) {
            unsigned maxMap = 0, maxCoord = 0;
            ++genHist[d.tev.numTexGens];
            for (unsigned t = 0; t < std::min<uint32_t>(d.tev.numTexGens, 8); ++t)
                ++srcHist[d.xf.texGen[t].sourceRow];
            for (unsigned s = 0; s < std::min<uint32_t>(d.tev.numStages, 16); ++s) {
                if (!d.tev.stage[s].texEnable)
                    continue;
                maxMap = std::max<unsigned>(maxMap, d.tev.stage[s].texmap);
                maxCoord = std::max<unsigned>(maxCoord, d.tev.stage[s].texcoord);
                // Which UNIT each enabled stage names, and whether that unit has a texture bound.
                // Binding four units scored WORSE than pinning every stage to unit 0, so the
                // question is whether the units above 0 actually carry the image the stage wants.
                ++mapHist[d.tev.stage[s].texmap];
                if (d.tev.stage[s].texmap < 8 && d.tex[d.tev.stage[s].texmap].addr == 0)
                    ++unboundStages;
            }
            if (maxMap > 0)
                ++multiMap;
            if (maxCoord > 0)
                ++multiCoord;
            if (d.tev.numTexGens > 1)
                ++multiGen;
            // The alpha test is only doing work where it is not ALWAYS with both comparisons.
            if (d.tev.alphaOp0 != 7 || d.tev.alphaOp1 != 7)
                ++alphaTested;
            ++coordHist[maxCoord];
        }
        // How FAR the scene goes decides the vertex layout: carrying eight coordinate sets costs
        // 56 bytes a vertex, and carrying two costs 8. Measure rather than provision for GX's
        // maximum. The texgen SOURCE histogram matters just as much — a source of GEOM or NORMAL
        // means the coordinate is computed from the vertex, not read from the mesh at all.
        for (const auto& [k, n] : coordHist)
            lucent::info("nrender", "    max texcoord index {} -> {} drawables", k, n);
        for (const auto& [k, n] : genHist)
            lucent::info("nrender", "    numTexGens {} -> {} drawables", k, n);
        for (const auto& [k, n] : srcHist)
            lucent::info("nrender", "    texgen source row {} -> {} texgens", k, n);
        // THE numStages-OVERRUN TEST. A material's own state arrives in one burst, so the TREF
        // registers it wrote carry a stamp close to its texture binds. Any stage the loop reads
        // whose TREF register is stamped much earlier belongs to a PREVIOUS material — that stage's
        // texmap names a unit this material never bound, which is the surviving explanation for
        // the multi-texmap regression.
        {
            int overrun = 0, checked = 0;
            long staleStages = 0, totalStages = 0;
            for (const auto& d : g_cur.items) {
                uint32_t newest = 0;
                for (unsigned k = 0; k < 8; ++k)
                    newest = std::max(newest, d.tex[k].bindSeq);
                if (newest == 0)
                    continue;
                ++checked;
                bool any = false;
                const unsigned ns = std::min<uint32_t>(d.tev.numStages, 16);
                for (unsigned s = 0; s < ns; ++s) {
                    ++totalStages;
                    // 8 binds is comfortably wider than one material's burst (unit 0's measured
                    // lag never exceeds 3), so this does not flag a material's own writes.
                    if (newest - d.tev.trefSeq[s / 2] > 8) {
                        ++staleStages;
                        any = true;
                    }
                }
                if (any)
                    ++overrun;
            }
            lucent::info("nrender",
                         "  numStages overrun: {}/{} drawables read at least one stage "
                         "whose RAS1_TREF is from an earlier material ({} of {} stages)",
                         overrun, checked, staleStages, totalStages);
        }

        for (const auto& [k, n] : mapHist)
            lucent::info("nrender", "    stage names texmap {} -> {} enabled stages", k, n);
        lucent::info("nrender", "    {} enabled stages name a unit with NO texture bound",
                     unboundStages);
        // How many DISTINCT images each unit carries across the tick. Unit 0 varies per material
        // (83 distinct textures measured). If units 1-3 show only a handful, they are STALE —
        // holding whatever the last material to bind them left there — and a stage that names one
        // samples another material's texture. That is the difference between "bound" and "correct",
        // and it is the only thing separating the two bisect configurations.
        for (unsigned m = 0; m < 8; ++m) {
            std::unordered_map<uint32_t, int> uh;
            long lagSum = 0, lagMax = 0, n = 0;
            for (const auto& d : g_cur.items) {
                ++uh[d.tex[m].addr];
                // How many binds happened between this unit's last bind and the most recent bind
                // of ANY unit at capture time. Zero means the unit was bound as part of this
                // drawable's own material; a large lag means it still holds an earlier material's
                // texture, which is what "stale" actually means.
                uint32_t newest = 0;
                for (unsigned k = 0; k < 8; ++k)
                    newest = std::max(newest, d.tex[k].bindSeq);
                if (newest == 0)
                    continue;
                const long lag = (long)newest - (long)d.tex[m].bindSeq;
                lagSum += lag;
                lagMax = std::max(lagMax, lag);
                ++n;
            }
            lucent::info("nrender",
                         "    unit {}: {} distinct addresses, bind lag mean {:.1f} max {}", m,
                         uh.size(), n ? (double)lagSum / (double)n : 0.0, lagMax);
        }
        lucent::info("nrender",
                     "  texture units: {} drawables reference texmap>0, {} texcoord>0, "
                     "{} declare >1 texgen; {} use the alpha test (of {})",
                     multiMap, multiCoord, multiGen, alphaTested, g_cur.items.size());
    }

    // Blend state too: an unexpected factor pair washes the frame toward the clear colour just as
    // convincingly as a bad alpha, and the two are indistinguishable in the output.
    std::unordered_map<uint32_t, int> bhist;
    for (const auto& d : g_cur.items)
        ++bhist[(uint32_t)d.depth.blend << 8 | (uint32_t)d.depth.srcFac << 4 | d.depth.dstFac];
    for (const auto& [k, n] : bhist)
        lucent::info("nrender", "  blend mode={} src={} dst={} -> {} drawables", (k >> 8) & 3,
                     (k >> 4) & 7, k & 7, n);
}

void sbr_scene_report_largest(int n) {
    std::vector<Area> a = g_area;
    std::sort(a.begin(), a.end(), [](const Area& x, const Area& y) { return x.frac > y.frac; });
    for (int i = 0; i < n && i < (int)a.size(); ++i) {
        // Report the drawable's COLOUR too. The colour is a pure function of the key, so a dominant
        // colour in the output identifies its drawable exactly — turning "something covers the
        // screen" into "THIS shape covers the screen", without another round of hypotheses.
        uint64_t z = a[i].key + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        const uint32_t hh = (uint32_t)(z ^ (z >> 31));
        const int cr = (int)(255.0f * (0.35f + 0.65f * (float)((hh >> 16) & 0xFF) / 255.0f));
        const int cg = (int)(255.0f * (0.35f + 0.65f * (float)((hh >> 8) & 0xFF) / 255.0f));
        const int cb = (int)(255.0f * (0.35f + 0.65f * (float)(hh & 0xFF) / 255.0f));
        lucent::info("nrender",
                     "  occluder #{}: shape 0x{:08x} el {} covers {:5.1f}%, {} verts, "
                     "nearest w={:.1f}, colour ({},{},{})",
                     i + 1, (uint32_t)(a[i].key >> 16), (uint32_t)(a[i].key & 0xFFFF),
                     100.0f * a[i].frac, a[i].verts, a[i].nearestW, cr, cg, cb);
    }
}

void sbr_scene_begin_tick() {
    g_projThisTick = false;
    g_building.items.clear();
    g_building.index.clear();
}

unsigned long g_addsIs2d =
    0; // adds with is2d set, counted AT THE ADD — pairs with end_tick's count

void sbr_scene_add(const SbrDrawable& d) {
    if (d.is2d)
        ++g_addsIs2d;
    g_building.index.emplace(d.key, (uint32_t)g_building.items.size());
    g_building.items.push_back(d);
}

void sbr_scene_end_tick() {
    { // WHERE do the FIFO-2D drawables go? The gate telemetry proves ~75 emits per frame reach
        // sbr_scene_add, yet the census over g_cur never sees an orthographic drawable. Count the
        // populations at the exact moment of rotation, so "lost between add and census" becomes a
        // location rather than a mystery.
        static long tick = 0;
        if (++tick % 300 == 0) {
            size_t ortho = 0;
            for (const auto& d : g_building.items)
                if (d.proj[11] == 0.0f)
                    ++ortho;
            lucent::info(
                "nrender",
                "  end_tick: rotating {} items ({} ortho, {} is2d) into "
                "g_cur; {} is2d adds SEEN by this scene instance since start",
                g_building.items.size(), ortho,
                [&] {
                    size_t n = 0;
                    for (const auto& d : g_building.items)
                        n += d.is2d;
                    return n;
                }(),
                g_addsIs2d);
        }
    }
    g_prev = std::move(g_cur);
    g_cur = std::move(g_building);
    g_building = Snapshot{};

    // Stamp with the monotonic clock the renderer also reads.
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_cur.time = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    g_cur.valid = true;

    if (g_prev.valid) {
        const double dt = g_cur.time - g_prev.time;
        // Ignore absurd intervals (a load hitch, or the first tick after a stage change) rather
        // than letting one of them poison the period the interpolation runs at.
        if (dt > 0.002 && dt < 0.2)
            g_tickPeriod = g_tickPeriod * 0.9 + dt * 0.1;
    }

    g_lastCount = (int)g_cur.items.size();
    g_matched = 0;
    if (g_prev.valid)
        for (const auto& d : g_cur.items)
            if (g_prev.index.count(d.key))
                ++g_matched;
}

// Which rendered tick this is. Only the per-draw state dump uses it, and only to pick a tick that
// actually has the scene in it.
long g_tickIndex = 0;

namespace {
struct PendingCopy {
    uint32_t streamPos;
    uint32_t dest;
    int sx, sy, sw, sh, dw, dh;
};
std::vector<PendingCopy> g_pendingCopies;
} // namespace

// Called from the FIFO parser when a copy-to-texture is triggered. The capture order at that
// moment is the position in the batch list where the copy belongs.
void sbr_scene_clear_pending_copies() {
    g_pendingCopies.clear();
}

void sbr_scene_note_efb_copy(uint32_t dest, int sx, int sy, int sw, int sh, int dw, int dh) {
    g_pendingCopies.push_back({sbr_gxfifo_stream_pos(), dest, sx, sy, sw, sh, dw, dh});
    static long n = 0;
    if (++n <= 4)
        lucent::info("nrender", "note EFB copy 0x{:08x} after drawable {} ({}x{} -> {}x{})", dest,
                     g_cur.items.size(), sw, sh, dw, dh);
}

float sbr_scene_render(double now_seconds, const float proj[16]) {
    g_lastFrameDraws = sbr_gxfifo_take_draw_count();
    if (!g_cur.valid)
        return 0.0f;
    ++g_tickIndex;

    // How far between the two snapshots this display frame falls. Derived from the WALL CLOCK, so
    // the render rate is independent of the tick rate — the whole point of the rework.
    float alpha = 1.0f;
    if (g_prev.valid && g_tickPeriod > 0.0) {
        alpha = (float)((now_seconds - g_cur.time) / g_tickPeriod);
        // Render one tick BEHIND: the scene is drawn between prev and cur, so it is always
        // interpolating between two KNOWN states rather than extrapolating past cur into a state
        // the game has not simulated. Extrapolation is what makes objects overshoot and snap back.
        alpha = std::clamp(alpha, 0.0f, 1.0f);
    }

    if (proj == nullptr)
        return alpha; // nothing to place geometry with

    g_area.clear();
    size_t drawableIdx = 0;
    struct CopyClear {
        ~CopyClear() { sbr_scene_clear_pending_copies(); }
    } copyClear;
    for (const auto& d : g_cur.items) {
        ++drawableIdx;
        if (d.geom == 0 || d.geom >= g_geom.size())
            continue; // no geometry decoded for this one
        const Geom& g = g_geom[d.geom];
        if (g.verts.empty())
            continue;
        // SBR_RENDER_SKIP_SKINNED=1 — omit shapes whose vertices select more than one matrix slot.
        // Those are the shapes whose single per-drawable matrix is KNOWN to be wrong (the slot read
        // from a multi-matrix J3DShapeMtx is not a slot at all), so this isolates how much of the
        // picture their garbage transforms are responsible for.
        if (g_skipSkinned && g.multislot)
            continue;

        float m[12];
        const auto it = g_prev.valid ? g_prev.index.find(d.key) : g_prev.index.end();
        if (it != g_prev.index.end()) {
            lerp_mtx(m, g_prev.items[it->second].mtx, d.mtx, alpha);
        } else {
            // New this tick: no previous state to come from. Draw it AT its current transform
            // rather than interpolating from whatever memory happened to hold — the same provenance
            // rule that the old approach needed a special case for, here a natural consequence.
            std::memcpy(m, d.mtx, sizeof m);
        }

        // Bring-up colouring: a deterministic hue per drawable so distinct objects are
        // distinguishable in the A/B against aurora. Materials and TEV are a later step; this is
        // explicitly NOT an attempt at the game's shading.
        // splitmix64 finaliser: a plain multiply truncated to 32 bits left the low bits zero for
        // every key whose element index was 0 — which is most shapes — so two of the three channels
        // were constant and the picture was unreadable.
        uint64_t z = d.key + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        const uint32_t h = (uint32_t)(z ^ (z >> 31));
        const float cr = 0.35f + 0.65f * (float)((h >> 16) & 0xFF) / 255.0f;
        const float cg = 0.35f + 0.65f * (float)((h >> 8) & 0xFF) / 255.0f;
        const float cb = 0.35f + 0.65f * (float)(h & 0xFF) / 255.0f;

        // Per-DRAWABLE buffer: depth state is a per-material property, so each drawable is
        // submitted under its own state. The backend merges consecutive runs that share a state,
        // so an unchanging scene still collapses to very few draws.
        g_out.clear();
        float nlo[2] = {1e30f, 1e30f}, nhi[2] = {-1e30f, -1e30f};
        float nearestW = 1e30f;
        for (const SbrGeomVert& v : g.verts) {
            // model -> view: the drawable's 3x4 is already model x view (J3D concats the view in
            // viewCalc), so this single multiply lands the vertex in eye space.
            const float vx = m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3];
            const float vy = m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7];
            const float vz = m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11];

            // The normal goes through the matrix's rotation only (no translation). J3D's draw
            // matrices are rigid plus uniform scale, so the 3x3 is its own inverse-transpose up to
            // a scale that renormalising removes.
            float nrm[3] = {m[0] * v.nx + m[1] * v.ny + m[2] * v.nz,
                            m[4] * v.nx + m[5] * v.ny + m[6] * v.nz,
                            m[8] * v.nx + m[9] * v.ny + m[10] * v.nz};
            const float nl = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
            if (nl > 1e-6f) {
                nrm[0] /= nl;
                nrm[1] /= nl;
                nrm[2] /= nl;
            }

            // view -> clip through the game's own projection.
            //
            // GC clip depth is NOT [0,1]. The game's own matrix (measured) has row 2 = [0, 0, 0,
            // -near], so z/w = -near/w: exactly -1 at the near plane and approaching 0 at infinity.
            // The convention is [-1, 0], near to far — an infinite-far projection.
            //
            // Vulkan (and so SDL3 GPU) wants [0, 1] and CLIPS anything with z < 0, so feeding GC's
            // z straight through discarded every surface near the camera and left only the distant
            // ones. That is what painted the sky and terrain over the whole plaza, and what made
            // the depth visualisation read 0 everywhere.
            SbrVertex o{};
            const float* P = d.proj;
            o.x = P[0] * vx + P[1] * vy + P[2] * vz + P[3];
            o.y = P[4] * vx + P[5] * vy + P[6] * vz + P[7];
            o.z = P[8] * vx + P[9] * vy + P[10] * vz + P[11];
            o.w = P[12] * vx + P[13] * vy + P[14] * vz + P[15];
            // NO Y FLIP. GC clip space and the backend's NDC agree in sign here, and the readback
            // already emits rows top-left first — negating y flipped the whole scene vertically.
            // It was invisible while everything was flat colour and obvious the moment textures
            // landed (paving on the ceiling, Mario upside down), which is a good reminder that a
            // flat-shaded render hides orientation bugs.
            // [-1,0] -> [0,1]: adding w maps near (-w) to 0 and far (0) to w. Same shape as the
            // standard GL->Vulkan depth conversion, and derived from the matrix above rather than
            // fitted to make the picture look right. GC's ORTHO shares the convention (C_MTXOrtho
            // maps near to -1 and far to 0), so the same correction applies to both.
            o.z += o.w;
            // Vertex colour from the decoded CLR0, modulated by the bring-up per-object hue only
            // when there is no texture — with a texture bound the object hue would tint the whole
            // scene and hide what the texture actually looks like.
            float vc[4] = {(float)((v.rgba >> 24) & 0xFF) / 255.0f,
                           (float)((v.rgba >> 16) & 0xFF) / 255.0f,
                           (float)((v.rgba >> 8) & 0xFF) / 255.0f, (float)(v.rgba & 0xFF) / 255.0f};
            const float vpos[3] = {vx, vy, vz};
            float lit[4];
            sbr_light_channel(d.xf, 0, vpos, nrm, vc, lit, nullptr);
            const float vr = lit[0], vg = lit[1], vb = lit[2], va = lit[3];
            if (g_depthViz) {
                // Grayscale by normalised depth: near = black, far = white. If a surface that
                // should be distant paints itself dark (or vice versa) the transform for that mesh
                // is wrong, which per-object colours cannot show.
                const float dz = (o.w > 1e-4f) ? std::clamp(o.z / o.w, 0.0f, 1.0f) : 1.0f;
                o.r = o.g = o.b = dz;
                o.a = 1.0f;
            } else if (d.tex[0].addr != 0) {
                o.r = vr;
                o.g = vg;
                o.b = vb;
                o.a = va;
                ++g_alphaTotal;
                if (va < 0.5f)
                    ++g_alphaLow;
                g_lumSum += (double)(0.2126f * vr + 0.7152f * vg + 0.0722f * vb);
                if (vr + vg + vb < 0.05f)
                    ++g_lumBlack;
            } else {
                o.r = cr * vr;
                o.g = cg * vg;
                o.b = cb * vb;
                o.a = va;
            }
            // One final coordinate per texgen. Texgens the material does not declare keep zero
            // rather than the raw set, so an out-of-range texcoord selector shows as a corner
            // sample instead of quietly looking plausible.
            for (unsigned tg = 0; tg < 4; ++tg) {
                if (!g_texgenOn) {
                    o.uv[tg][0] = v.uv[0][0];
                    o.uv[tg][1] = v.uv[0][1];
                } else if (tg < d.tev.numTexGens) {
                    sbr_texgen(d.xf, tg, v, lit, o.uv[tg]);
                } else {
                    o.uv[tg][0] = o.uv[tg][1] = 0.0f;
                }
            }
            // Channel 1 for this vertex, evaluated by the same reference the trace uses.
            {
                float lit1[4];
                if (d.xf.numChans > 1)
                    sbr_light_channel(d.xf, 1, vpos, nrm, vc, lit1, nullptr);
                else {
                    lit1[0] = vr;
                    lit1[1] = vg;
                    lit1[2] = vb;
                    lit1[3] = va;
                }
                o.r1 = lit1[0];
                o.g1 = lit1[1];
                o.b1 = lit1[2];
                o.a1 = lit1[3];
            }
            g_out.push_back(o);

            if (o.w > 1e-4f) {
                const float sx = o.x / o.w, sy = o.y / o.w;
                nlo[0] = std::min(nlo[0], sx);
                nhi[0] = std::max(nhi[0], sx);
                nlo[1] = std::min(nlo[1], sy);
                nhi[1] = std::max(nhi[1], sy);
                nearestW = std::min(nearestW, o.w);
            }
        }
        // PER-DRAW STATE, at the point of use. A whole-frame score says something is wrong but
        // never WHERE, so every step becomes a hypothesis costing a run; this reports the three
        // things a stage needs to sample — the unit it names, the texture bound to that unit, and
        // the coordinate it samples with — for the first SBR_DRAW_STATE drawables that have
        // geometry. The GX side of this is RE-verified against the game's own writers (J3DTevs.cpp
        // loadTexNo/JRNISetTevOrder + GXTexture.c GXLoadTexObjPreLoaded), so a disagreement here is
        // this renderer's, not the parser's.
        //
        // SBR_DRAW_STATE=<n> logs EVERY draw of tick n rather than the first n draws overall. The
        // first tick of a stage is the loading screen — a handful of 2D quads — so the scene whose
        // defect is being chased is not in it. Distance is on the line too (nearest clip w),
        // because the observed split is near-renders / distant-blacks and a state dump that cannot
        // tell the two apart cannot answer which state differs.
        static const long kDrawStateTick = [] {
            const char* e = std::getenv("SBR_DRAW_STATE");
            return e != nullptr ? std::strtol(e, nullptr, 10) : 0;
        }();
        static long drawStateSeen = 0;
        if (!g_out.empty() && kDrawStateTick > 0 && g_tickIndex == kDrawStateTick) {
            ++drawStateSeen;
            lucent::Line l;
            // The NDC bounding box, so a draw can be matched to a REGION of the dumped frame. That
            // is what turns "the sky is black" into "draw 412 is black": read the box off the line
            // instead of guessing which drawable covers the defect.
            l.add("draw {} verts={} box=[{:.2f},{:.2f}]x[{:.2f},{:.2f}] nearW={:.0f} zt{}w{}f{} "
                  "blend{} numTexGens={} numStages={} |",
                  drawStateSeen, g_out.size(), nlo[0], nhi[0], nlo[1], nhi[1], nearestW,
                  d.depth.test, d.depth.write, d.depth.func, d.depth.blend, d.tev.numTexGens,
                  d.tev.numStages);
            for (unsigned m = 0; m < 8; ++m)
                l.add(" u{}={:08x}/f{}/{}x{}@{}", m, d.tex[m].addr, d.tex[m].format, d.tex[m].width,
                      d.tex[m].height, d.tex[m].bindSeq);
            l.add(" |");
            // The stage's COMBINER too, not just which texture it names. GX computes
            // out = (d + sign*mix(a,b,c) + bias) * scale -> dest, and a stage whose result is zero
            // blacks everything downstream that reads it as CPREV. Printing the selectors means the
            // arithmetic can be read off the line instead of hypothesised and costing a run each.
            for (unsigned s = 0; s < std::min<uint32_t>(d.tev.numStages, 8); ++s) {
                const SbrTevStage& st = d.tev.stage[s];
                float k[4];
                sbr_tev_konst(d.tev, s, k);
                l.add(" s{}:map{}{} coord{} c(a{} b{} c{} d{} bias{} sub{} sc{} ->{}) a(a{} b{} c{}"
                      " d{} ->{}) k[{:.2f},{:.2f},{:.2f},{:.2f}]",
                      s, st.texmap, st.texEnable ? "" : "(off)", st.texcoord, st.cA, st.cB, st.cC,
                      st.cD, st.cBias, st.cSub, st.cScale, st.cDest, st.aA, st.aB, st.aC, st.aD,
                      st.aDest, k[0], k[1], k[2], k[3]);
            }
            l.add(" reg[{:.2f},{:.2f},{:.2f},{:.2f}]", d.tev.reg[0][0], d.tev.reg[1][0],
                  d.tev.reg[2][0], d.tev.reg[3][0]);
            l.add(" | tg");
            for (unsigned g2 = 0; g2 < 4; ++g2) {
                const SbrTexGen& tg2 = d.xf.texGen[g2];
                l.add(" {}:t{}/src{}/mtx{}{}", g2, tg2.type, tg2.sourceRow, tg2.mtxSlot,
                      (tg2.mtxSlot < 10 && !((d.xf.texMtxWritten >> tg2.mtxSlot) & 1)) ? "!" : "");
            }
            l.add(" | uv");
            for (unsigned tg = 0; tg < 4; ++tg) {
                float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
                for (const SbrVertex& o : g_out)
                    for (int c = 0; c < 2; ++c) {
                        lo[c] = std::min(lo[c], o.uv[tg][c]);
                        hi[c] = std::max(hi[c], o.uv[tg][c]);
                    }
                l.add(" {}:[{:.2f},{:.2f}]x[{:.2f},{:.2f}]", tg, lo[0], hi[0], lo[1], hi[1]);
            }
            l.flush(lucent::Level::Info, "nrender");
        }
        // SBR_TEV_TRACE=<tick>: for the first few drawables of that tick, EXPLAIN the pixel — run
        // the reference TEV evaluator (runtime/render/tev_eval.cpp, unit-tested against the SDK) on
        // this drawable's real state and its real decoded texels, and print every stage's inputs
        // and output. "This surface is black" becomes "stage 3 wrote 0 to PREV because C2 was 0".
        // No frame score, no inference, no GPU.
        static const long kTraceTick = [] {
            const char* e = std::getenv("SBR_TEV_TRACE");
            return e != nullptr ? std::strtol(e, nullptr, 10) : 0;
        }();
        // SBR_TEV_TRACE_BLACK=1 makes the trace SELF-SELECT the defect: evaluate every drawable of
        // the tick, print only the ones that come out black. That is the difference between a tool
        // that shows six arbitrary surfaces and one that answers "why is THIS black".
        static const bool kTraceBlack = [] {
            const char* e = std::getenv("SBR_TEV_TRACE_BLACK");
            return e != nullptr && e[0] != '\0' && e[0] != '0';
        }();
        static long traced = 0;
        if (!g_out.empty() && kTraceTick > 0 && g_tickIndex == kTraceTick && traced < 6) {
            SbrTevInputs in{};
            // The rasterised colour and the coordinates of the first vertex — one representative
            // fragment of this surface, which is enough to explain a uniformly black one.
            const SbrVertex& v0 = g_out[0];
            in.ras[0] = v0.r;
            in.ras[1] = v0.g;
            in.ras[2] = v0.b;
            in.ras[3] = v0.a;
            // Colour channel 1 for the same representative vertex, so a stage whose RAS selector
            // names channel 1 evaluates against what the hardware would rasterise, not channel 0.
            {
                const SbrGeomVert& gv = g.verts[0];
                const float vp[3] = {m[0] * gv.x + m[1] * gv.y + m[2] * gv.z + m[3],
                                     m[4] * gv.x + m[5] * gv.y + m[6] * gv.z + m[7],
                                     m[8] * gv.x + m[9] * gv.y + m[10] * gv.z + m[11]};
                float nn[3] = {m[0] * gv.nx + m[1] * gv.ny + m[2] * gv.nz,
                               m[4] * gv.nx + m[5] * gv.ny + m[6] * gv.nz,
                               m[8] * gv.nx + m[9] * gv.ny + m[10] * gv.nz};
                const float nl = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                if (nl > 1e-6f) {
                    nn[0] /= nl;
                    nn[1] /= nl;
                    nn[2] /= nl;
                }
                const float gvc[4] = {(float)((gv.rgba >> 24) & 0xFF) / 255.0f,
                                      (float)((gv.rgba >> 16) & 0xFF) / 255.0f,
                                      (float)((gv.rgba >> 8) & 0xFF) / 255.0f,
                                      (float)(gv.rgba & 0xFF) / 255.0f};
                sbr_light_channel(d.xf, 1, vp, nn, gvc, in.ras1, nullptr);
            }
            // Channel 0 across the WHOLE drawable, not just vertex 0. The trace's verdict on one
            // vertex generalises only if the channel is black everywhere — a surface with mixed
            // facing would be partly lit, and a "black" conclusion drawn from one back-facing
            // vertex would be the instrument choosing the answer.
            float ras0Mean = 0.0f, ras0Max = 0.0f;
            {
                size_t nv = 0;
                for (const SbrVertex& vv : g_out) {
                    const float lum = 0.2126f * vv.r + 0.7152f * vv.g + 0.0722f * vv.b;
                    ras0Mean += lum;
                    ras0Max = std::max(ras0Max, lum);
                    ++nv;
                }
                if (nv > 0)
                    ras0Mean /= (float)nv;
            }
            for (unsigned m = 0; m < 8; ++m) {
                const SbrTexture& t = d.tex[m];
                if (t.addr == 0 || t.width == 0 || t.height == 0)
                    continue;
                std::vector<uint8_t> rgba((size_t)t.width * t.height * 4);
                if (!gx_decode_texture(t.addr, t.width, t.height, t.format, t.tlut, rgba.data()))
                    continue;
                // Sample with the coordinate the stage naming this unit actually uses, wrapped —
                // sampling the corner instead would explain a different pixel than the one drawn.
                unsigned coord = 0;
                for (unsigned st = 0; st < d.tev.numStages && st < 16; ++st)
                    if (d.tev.stage[st].texEnable && (d.tev.stage[st].texmap & 7) == m) {
                        coord = d.tev.stage[st].texcoord & 3;
                        break;
                    }
                float u = v0.uv[coord][0] - std::floor(v0.uv[coord][0]);
                float vv = v0.uv[coord][1] - std::floor(v0.uv[coord][1]);
                const uint32_t px = std::min<uint32_t>((uint32_t)(u * (float)t.width), t.width - 1);
                const uint32_t py =
                    std::min<uint32_t>((uint32_t)(vv * (float)t.height), t.height - 1);
                const uint8_t* p = &rgba[((size_t)py * t.width + px) * 4];
                for (int c = 0; c < 4; ++c)
                    in.tex[m][c] = (float)p[c] / 255.0f;
            }
            float outc[4];
            bool discarded = false;
            SbrTevTrace tr{};
            sbr_tev_evaluate(d.tev, in, outc, &discarded, &tr);
            const float lum = 0.2126f * outc[0] + 0.7152f * outc[1] + 0.0722f * outc[2];
            if (kTraceBlack && lum > 0.02f && !discarded)
                continue;
            // SBR_TEV_TRACE_FAR=<minNearW> selects a DISTANT drawable that names a unit above 0,
            // instead of self-selecting on a black vertex 0. The black self-selection kept picking
            // NEAR back-facing vertices — the trace choosing its own answer — while the surfaces
            // that actually go black under named units are distant. Population matters more than
            // the predicate.
            static const float kFar = [] {
                const char* e = std::getenv("SBR_TEV_TRACE_FAR");
                return e != nullptr ? (float)std::atof(e) : 0.0f;
            }();
            if (kFar > 0.0f) {
                if (nearestW < kFar)
                    continue;
                bool namesUpper = false;
                for (unsigned st = 0; st < d.tev.numStages && st < 16; ++st)
                    if (d.tev.stage[st].texEnable && (d.tev.stage[st].texmap & 7) > 0)
                        namesUpper = true;
                if (!namesUpper)
                    continue;
            }
            // SBR_TEV_TRACE_ADDR=<hex guest addr>: trace only drawables where some enabled stage's
            // NAMED unit holds that texture. This is how one specific black batch (e.g. the ones
            // binding the zero CMPR on unit 1) is accounted end to end rather than whichever
            // drawable the other filters happen to reach first.
            static const uint32_t kAddr = [] {
                const char* e = std::getenv("SBR_TEV_TRACE_ADDR");
                return e != nullptr ? (uint32_t)std::strtoul(e, nullptr, 16) : 0u;
            }();
            if (kAddr != 0) {
                bool hit = false;
                for (unsigned st = 0; st < d.tev.numStages && st < 16; ++st)
                    if (d.tev.stage[st].texEnable &&
                        d.tex[d.tev.stage[st].texmap & 7].addr == kAddr)
                        hit = true;
                if (!hit)
                    continue;
            }
            ++traced;
            lucent::info("tev",
                         "drawable {} verts={} numStages={} ras[{:.3f} {:.3f} {:.3f} a{:.3f}] "
                         "ndc x[{:.2f},{:.2f}] y[{:.2f},{:.2f}] nearW={:.0f}",
                         traced, g_out.size(), d.tev.numStages, in.ras[0], in.ras[1], in.ras[2],
                         in.ras[3], nlo[0], nhi[0], nlo[1], nhi[1], nearestW);
            { // WHICH channel each stage reads as RASC, and through WHICH swap row. The GPU
                // shader still ignores the selector and feeds colour channel 0; the evaluator
                // above honours both, so a divergence between this trace and the frame now points
                // at the shader, not the reference.
                lucent::Line rl;
                rl.add("  ras per stage:");
                for (unsigned st = 0; st < d.tev.numStages && st < 16; ++st) {
                    const SbrTevStage& s = d.tev.stage[st];
                    const uint8_t* sw = d.tev.swapTable[s.swapRas & 3];
                    static const char comp[4] = {'R', 'G', 'B', 'A'};
                    rl.add(" s{}=ch{}/{}{}{}{}", st, s.rasChannel, comp[sw[0] & 3], comp[sw[1] & 3],
                           comp[sw[2] & 3], comp[sw[3] & 3]);
                }
                rl.flush(lucent::Level::Info, "tev");
                rl.add("  ras1 (channel 1) at vertex0 = [{:.3f} {:.3f} {:.3f} a{:.3f}]; "
                       "chan0 luma over {} verts: mean {:.3f} max {:.3f}",
                       in.ras1[0], in.ras1[1], in.ras1[2], in.ras1[3], g_out.size(), ras0Mean,
                       ras0Max);
                rl.flush(lucent::Level::Info, "tev");
            }
            for (unsigned m = 0; m < 8; ++m)
                lucent::info("tev",
                             "  unit {} = 0x{:08x} {}x{} fmt{} texel[{:.3f} {:.3f} {:.3f} "
                             "a{:.3f}]{}",
                             m, d.tex[m].addr, d.tex[m].width, d.tex[m].height, d.tex[m].format,
                             in.tex[m][0], in.tex[m][1], in.tex[m][2], in.tex[m][3],
                             sbr_render_is_copy_surface(d.tex[m].addr)
                                 ? "   <- EFB COPY SURFACE: the GPU samples the rendered surface "
                                   "here; this reference decoded GUEST MEMORY and its texel is NOT "
                                   "what was sampled"
                                 : "");
            // WHY the rasterised colour is what it is. A black RAS makes every stage that reads it
            // produce black no matter how well the textures decoded, so the channel has to explain
            // itself in the same trace — otherwise the search moves to textures for the wrong
            // reason.
            {
                const SbrChanCtrl& c = d.xf.chan[0];
                lucent::info("tev",
                             "  chan0: light={} matSrc={} ambSrc={} diffFn={} attn={}/{} "
                             "mask=0x{:02x} numChans={}",
                             c.enableLight, c.matSrcVertex ? "vtx" : "reg",
                             c.ambSrcVertex ? "vtx" : "reg", c.diffuseFn, c.attnEnable, c.attnSpot,
                             c.lightMask, d.xf.numChans);
                lucent::info("tev",
                             "  chan0: matReg[{:.3f} {:.3f} {:.3f} a{:.3f}] "
                             "ambReg[{:.3f} {:.3f} {:.3f}]",
                             d.xf.material[0][0], d.xf.material[0][1], d.xf.material[0][2],
                             d.xf.material[0][3], d.xf.ambient[0][0], d.xf.ambient[0][1],
                             d.xf.ambient[0][2]);
                for (int li = 0; li < 8; ++li) {
                    if (!((c.lightMask >> li) & 1))
                        continue;
                    const SbrLight& L = d.xf.light[li];
                    lucent::info("tev",
                                 "  light {}: col[{:.3f} {:.3f} {:.3f}] pos[{:.0f} {:.0f} "
                                 "{:.0f}] dir[{:.3f} {:.3f} {:.3f}] cosAtt[{:.3f} {:.3f} "
                                 "{:.3f}] distAtt[{:.4f} {:.4f} {:.4f}]",
                                 li, L.color[0], L.color[1], L.color[2], L.pos[0], L.pos[1],
                                 L.pos[2], L.dir[0], L.dir[1], L.dir[2], L.cosAtt[0], L.cosAtt[1],
                                 L.cosAtt[2], L.distAtt[0], L.distAtt[1], L.distAtt[2]);
                }
                // The same first vertex the texels were sampled at, so the channel working below
                // explains the very RAS printed above rather than some other fragment.
                {
                    const SbrGeomVert& gv = g.verts[0];
                    const float vx = m[0] * gv.x + m[1] * gv.y + m[2] * gv.z + m[3];
                    const float vy = m[4] * gv.x + m[5] * gv.y + m[6] * gv.z + m[7];
                    const float vz = m[8] * gv.x + m[9] * gv.y + m[10] * gv.z + m[11];
                    float nn[3] = {m[0] * gv.nx + m[1] * gv.ny + m[2] * gv.nz,
                                   m[4] * gv.nx + m[5] * gv.ny + m[6] * gv.nz,
                                   m[8] * gv.nx + m[9] * gv.ny + m[10] * gv.nz};
                    const float nl = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                    if (nl > 1e-6f) {
                        nn[0] /= nl;
                        nn[1] /= nl;
                        nn[2] /= nl;
                    }
                    const float vp[3] = {vx, vy, vz};
                    lucent::info("tev",
                                 "  vertex0: model pos[{:.0f} {:.0f} {:.0f}] model normal"
                                 "[{:.3f} {:.3f} {:.3f}] (len {:.3f}) -> view normal"
                                 "[{:.3f} {:.3f} {:.3f}]",
                                 gv.x, gv.y, gv.z, gv.nx, gv.ny, gv.nz,
                                 std::sqrt(gv.nx * gv.nx + gv.ny * gv.ny + gv.nz * gv.nz), nn[0],
                                 nn[1], nn[2]);
                    float gvc[4] = {(float)((gv.rgba >> 24) & 0xFF) / 255.0f,
                                    (float)((gv.rgba >> 16) & 0xFF) / 255.0f,
                                    (float)((gv.rgba >> 8) & 0xFF) / 255.0f,
                                    (float)(gv.rgba & 0xFF) / 255.0f};
                    light_channel_trace(d.xf, 0, vp, nn, gvc, "tev");
                }
            }
            // PER-STAGE TEXEL A/B: what each stage samples from the unit it NAMES, against what it
            // would sample from unit 0 — the only thing the texmap routing changes. Averaged over
            // many vertices, because a single vertex has now chosen the answer twice in this arc.
            if (kFar > 0.0f) {
                std::vector<std::vector<uint8_t>> dec(4);
                std::vector<std::pair<uint32_t, uint32_t>> dim(4, {0, 0});
                for (unsigned mm = 0; mm < 8; ++mm) {
                    const SbrTexture& t = d.tex[mm];
                    if (t.addr == 0 || t.width == 0 || t.height == 0)
                        continue;
                    std::vector<uint8_t> rgba((size_t)t.width * t.height * 4);
                    if (!gx_decode_texture(t.addr, t.width, t.height, t.format, t.tlut,
                                           rgba.data()))
                        continue;
                    dec[mm] = std::move(rgba);
                    dim[mm] = {t.width, t.height};
                }
                auto sampleAt = [&](unsigned unit, const SbrVertex& vx, unsigned coord,
                                    float out[4]) {
                    out[0] = out[1] = out[2] = out[3] = -1.0f;
                    if (unit > 3 || dec[unit].empty())
                        return;
                    const float uu = vx.uv[coord][0] - std::floor(vx.uv[coord][0]);
                    const float vv2 = vx.uv[coord][1] - std::floor(vx.uv[coord][1]);
                    const uint32_t px = std::min<uint32_t>((uint32_t)(uu * (float)dim[unit].first),
                                                           dim[unit].first - 1);
                    const uint32_t py = std::min<uint32_t>(
                        (uint32_t)(vv2 * (float)dim[unit].second), dim[unit].second - 1);
                    const uint8_t* q = &dec[unit][((size_t)py * dim[unit].first + px) * 4];
                    for (int c = 0; c < 4; ++c)
                        out[c] = (float)q[c] / 255.0f;
                };
                const size_t step = std::max<size_t>(1, g_out.size() / 32);
                for (unsigned st = 0; st < d.tev.numStages && st < 16; ++st) {
                    if (!d.tev.stage[st].texEnable)
                        continue;
                    const unsigned named = d.tev.stage[st].texmap & 7;
                    const unsigned coord = d.tev.stage[st].texcoord & 3;
                    float nsum[4] = {0, 0, 0, 0}, psum[4] = {0, 0, 0, 0};
                    size_t n = 0;
                    for (size_t vi = 0; vi < g_out.size(); vi += step) {
                        float a[4], b[4];
                        sampleAt(named, g_out[vi], coord, a);
                        sampleAt(0, g_out[vi], coord, b);
                        if (a[0] < 0.0f || b[0] < 0.0f)
                            continue;
                        for (int c = 0; c < 4; ++c) {
                            nsum[c] += a[c];
                            psum[c] += b[c];
                        }
                        ++n;
                    }
                    if (n == 0) {
                        lucent::info("tev",
                                     "  A/B s{}: unit {} has no decoded texture — the named "
                                     "routing samples NOTHING here",
                                     st, named);
                        continue;
                    }
                    for (int c = 0; c < 4; ++c) {
                        nsum[c] /= (float)n;
                        psum[c] /= (float)n;
                    }
                    lucent::info("tev",
                                 "  A/B s{} coord{}: named unit {} -> [{:.3f} {:.3f} {:.3f} "
                                 "a{:.3f}]   unit 0 -> [{:.3f} {:.3f} {:.3f} a{:.3f}]  "
                                 "(mean of {} verts)",
                                 st, coord, named, nsum[0], nsum[1], nsum[2], nsum[3], psum[0],
                                 psum[1], psum[2], psum[3], n);
                }
            }
            sbr_tev_trace_report(tr, "tev");
        }
        // Perform any EFB copy captured at or before this point in the draw order, here at the
        // actual submission site — the copy must capture only what was drawn before it.
        for (auto& pc : g_pendingCopies)
            if (pc.dest != 0 && pc.streamPos <= d.streamPos) {
                sbr_render_note_copy(pc.dest, pc.sx, pc.sy, pc.sw, pc.sh, pc.dw, pc.dh);
                pc.dest = 0;
            }
        if (!g_out.empty())
            sbr_render_tris(g_out.data(), (int)(g_out.size() / 3) * 3, d.depth, d.tex, d.tev);

        if (nhi[0] > nlo[0]) {
            // Clamp to the viewport before measuring: an off-screen quad's raw NDC extent is huge
            // but it covers nothing, and ranking by that would blame the wrong drawable.
            const float w0 = std::min(nhi[0], 1.0f) - std::max(nlo[0], -1.0f);
            const float h0 = std::min(nhi[1], 1.0f) - std::max(nlo[1], -1.0f);
            if (w0 > 0.0f && h0 > 0.0f)
                g_area.push_back(Area{d.key, w0 * h0 * 0.25f, (int)g.verts.size(), nearestW});
        }
    }

    return alpha;
}

void sbr_scene_translation_bounds(float lo[3], float hi[3], float* medianDist) {
    for (int i = 0; i < 3; ++i) {
        lo[i] = 1e30f;
        hi[i] = -1e30f;
    }
    std::vector<float> d;
    d.reserve(g_cur.items.size());
    for (const auto& it : g_cur.items) {
        const float t[3] = {it.mtx[3], it.mtx[7], it.mtx[11]};
        for (int i = 0; i < 3; ++i) {
            lo[i] = std::min(lo[i], t[i]);
            hi[i] = std::max(hi[i], t[i]);
        }
        d.push_back(std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]));
    }
    if (d.empty()) {
        *medianDist = 0.0f;
        return;
    }
    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
    *medianDist = d[d.size() / 2];
}

int sbr_scene_last_count() {
    return g_lastCount;
}
int sbr_scene_matched_count() {
    return g_matched;
}
