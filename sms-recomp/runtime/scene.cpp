// scene.cpp — the interpolated scene (see scene.h for why this replaced the tick-driven approach).

#include "scene.h"

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
    std::unordered_map<uint64_t, uint32_t> index;   // key -> position in items
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
    float    frac;      // fraction of the viewport covered by the projected bounding box
    int      verts;
    float    nearestW;  // smallest clip w = closest approach to the camera
};
std::vector<Area> g_area;

struct Geom {
    std::vector<SbrGeomVert> verts;
    bool multislot = false;
};
std::vector<Geom> g_geom{1};                       // [0] unused: handle 0 = none
std::unordered_map<uint64_t, uint32_t> g_geomIndex;
int g_multislot = 0;
int g_splitTris = 0;
long g_alphaLow = 0, g_alphaTotal = 0, g_lumBlack = 0;
double g_lumSum = 0.0;

float g_proj[16];
bool  g_projValid = false;
bool  g_projThisTick = false;

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

// GX colour channel: the RASTERISED colour a TEV stage reads as RASC/RASA. Without this the
// channel defaults to white, and every material that modulates by it blows out — which is exactly
// what characters did once TEV landed (the environment uses baked CLR0 and was unaffected).
//
// GX evaluates, per channel: colour = material * (ambient + sum over enabled lights of
// atten * diffuse * lightColour), where the material and ambient come either from a register or
// from the vertex, per the channel control. Lights live in VIEW space, which is where the draw
// matrix has already put the vertex and its normal.
void light_channel(const SbrXfState& xf, int chan, const float vpos[3], const float nrm[3],
                   const float vtxColor[4], float out[4]) {
    const SbrChanCtrl& c = xf.chan[chan];

    const float* mat = c.matSrcVertex ? vtxColor : xf.material[chan];
    if (!c.enableLight) {
        for (int i = 0; i < 4; ++i) out[i] = mat[i];
        return;
    }
    const float* amb = c.ambSrcVertex ? vtxColor : xf.ambient[chan];

    float acc[3] = {amb[0], amb[1], amb[2]};
    for (int li = 0; li < 8; ++li) {
        if (!((c.lightMask >> li) & 1)) continue;
        const SbrLight& L = xf.light[li];

        float ldir[3] = {L.pos[0] - vpos[0], L.pos[1] - vpos[1], L.pos[2] - vpos[2]};
        const float d2 = ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2];
        const float d = std::sqrt(d2);
        if (d > 1e-6f) { ldir[0] /= d; ldir[1] /= d; ldir[2] /= d; }

        float atten = 1.0f;
        if (c.attnEnable) {
            // Spotlight: the cosine polynomial over the angle to the light's direction, divided by
            // the distance polynomial. GX evaluates both as quadratics in their own variable.
            const float cosA = -(ldir[0] * L.dir[0] + ldir[1] * L.dir[1] + ldir[2] * L.dir[2]);
            const float num = std::max(0.0f, L.cosAtt[0] + L.cosAtt[1] * cosA +
                                                 L.cosAtt[2] * cosA * cosA);
            const float den = L.distAtt[0] + L.distAtt[1] * d + L.distAtt[2] * d2;
            atten = (den > 1e-6f) ? num / den : 0.0f;
        }

        float diff = 1.0f;
        if (c.diffuseFn != 0) {
            diff = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
            if (c.diffuseFn == 2) diff = std::max(0.0f, diff);   // clamp
        }
        const float k = atten * diff;
        acc[0] += k * L.color[0];
        acc[1] += k * L.color[1];
        acc[2] += k * L.color[2];
    }
    for (int i = 0; i < 3; ++i) out[i] = std::clamp(mat[i] * acc[i], 0.0f, 1.0f);
    out[3] = mat[3];
}

// GX TEXTURE COORDINATE GENERATION. The hardware never hands a stored coordinate straight to the
// sampler: each texgen picks a SOURCE row (a stored coordinate set, but equally the vertex's
// position or normal — that is how environment mapping is expressed) and multiplies it by one of
// the ten texture matrices. Using the raw TEX0 for every stage is only correct for the identity
// case; a scrolling or animated SRT lives entirely in that matrix.
//
// The source is the RAW, model-space vertex, which is what the XF sees: the position matrix is
// applied on the other path, and a texgen that wanted view space gets it because the game folds
// the view transform into the texture matrix itself.
void texgen(const SbrXfState& xf, unsigned g, const SbrGeomVert& v, const float vtxColor[4],
            float out[2]) {
    const SbrTexGen& tg = xf.texGen[g];

    // The texgen TYPE is decided before the source row. The two colour types do not go through a
    // texture matrix at all: the hardware emits the colour channel's red and green as the
    // coordinate directly, which is how a material looks up a ramp with its own lit colour.
    if (tg.type == 2 || tg.type == 3) {
        if (tg.type == 3) {
            // Channel 1 is not evaluated by this port yet, so a COLOR1 texgen would silently
            // sample with channel 0's colour and look like a shading bug.
            static bool warned = false;
            if (!warned) {
                warned = true;
                lucent::error("nrender", "texgen {} sources COLOR1, but only colour channel 0 is "
                                         "evaluated — its coordinate is channel 0's", g);
            }
        }
        out[0] = vtxColor[0];
        out[1] = vtxColor[1];
        return;
    }
    if (tg.type == 1) {
        // EMBOSS needs the binormal/tangent pair and a light, none of which this path decodes.
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("nrender", "texgen {} is EMBOSS (bump mapping) — not implemented; its "
                                     "coordinate is the un-offset source", g);
        }
    }

    float in[4] = {0, 0, 1, 1};
    switch (tg.sourceRow) {
    case 0: in[0] = v.x;  in[1] = v.y;  in[2] = v.z;  break;   // GEOM
    case 1: in[0] = v.nx; in[1] = v.ny; in[2] = v.nz; break;   // NORMAL
    case 2: in[0] = vtxColor[0]; in[1] = vtxColor[1]; break;   // COLORS
    default:
        // TEX0..TEX7 are rows 5..12. Sets beyond what the decoder reads keep (0,0), which the
        // decoder has already reported by name rather than silently substituting set 0.
        if (tg.sourceRow >= 5 && tg.sourceRow - 5 < 4) {
            in[0] = v.uv[tg.sourceRow - 5][0];
            in[1] = v.uv[tg.sourceRow - 5][1];
        }
        break;
    }
    // inputForm 0 is (a, b, 1, 1); 1 is (a, b, c, 1). Only the source rows that carry a third
    // component can distinguish them, and they set in[2] above.
    if (tg.inputForm == 0) in[2] = 1.0f;

    if (tg.mtxSlot >= 10) {   // GX_IDENTITY
        out[0] = in[0];
        out[1] = in[1];
        return;
    }
    if (!((xf.texMtxWritten >> tg.mtxSlot) & 1)) {
        // The slot was never written by a direct XF load, so its contents are zero, not identity.
        // Multiplying by it would map the whole surface to one texel — report the gap and pass the
        // source through, which is at least the coordinate the mesh stored.
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("nrender", "texgen {} selects texture matrix {}, which no direct XF load "
                                     "has written (GX also loads these by index — that path is not "
                                     "parsed); using the raw coordinate", g, tg.mtxSlot);
        }
        out[0] = in[0];
        out[1] = in[1];
        return;
    }
    const float* M = xf.texMtx[tg.mtxSlot];
    const float s = M[0] * in[0] + M[1] * in[1] + M[2]  * in[2] + M[3]  * in[3];
    const float t = M[4] * in[0] + M[5] * in[1] + M[6]  * in[2] + M[7]  * in[3];
    if (tg.projection == 1) {   // STQ: the third row is a perspective divide
        const float q = M[8] * in[0] + M[9] * in[1] + M[10] * in[2] + M[11] * in[3];
        const float inv = (q > 1e-6f || q < -1e-6f) ? 1.0f / q : 0.0f;
        out[0] = s * inv;
        out[1] = t * inv;
    } else {
        out[0] = s;
        out[1] = t;
    }
}

void lerp_mtx(float out[12], const float a[12], const float b[12], float t) {
    // Component-wise on a model x view matrix. Correct for translation and near-correct for the
    // rotation of a small per-tick delta; a proper decompose/slerp is the upgrade if fast spins
    // ever show shear, and is deliberately deferred rather than guessed at now.
    for (int i = 0; i < 12; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

} // namespace

bool sbr_scene_has_geometry(uint64_t key) { return g_geomIndex.count(key) != 0; }

uint32_t sbr_scene_intern_geometry(uint64_t key, const SbrGeomVert* verts, int count) {
    if (const auto it = g_geomIndex.find(key); it != g_geomIndex.end()) return it->second;

    const uint32_t id = (uint32_t)g_geom.size();
    g_geom.push_back(Geom{});
    Geom& g = g_geom.back();
    g.verts.assign(verts, verts + count);

    // A shape whose vertices select more than one matrix slot is SKINNED. The drawable carries one
    // matrix, so such a mesh renders with the wrong transform on every vertex outside the first
    // slot. Count it instead of hiding it: the per-vertex matrix path is the next step, and this
    // number is how big it is.
    for (int i = 1; i < count; ++i)
        if (verts[i].slot != verts[0].slot) { g.multislot = true; ++g_multislot; break; }

    g_geomIndex.emplace(key, id);
    return id;
}

uint32_t sbr_scene_geometry_for_slot(uint64_t baseKey, uint32_t baseGeom, uint32_t slot) {
    if (baseGeom == 0 || baseGeom >= g_geom.size()) return 0;
    const Geom& base = g_geom[baseGeom];
    // Unskinned fast path: every vertex is on one slot, so the element IS the slot's geometry.
    if (!base.multislot) return baseGeom;

    const uint64_t key = (baseKey << 8) | (uint64_t)slot | 0x8000000000000000ull;
    if (const auto it = g_geomIndex.find(key); it != g_geomIndex.end()) return it->second;

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
        if (base.verts[t].slot != slot) continue;
        sub.push_back(base.verts[t]);
        sub.push_back(base.verts[t + 1]);
        sub.push_back(base.verts[t + 2]);
    }
    if (sub.empty()) return 0;

    const uint32_t id = (uint32_t)g_geom.size();
    g_geom.push_back(Geom{});
    g_geom.back().verts = std::move(sub);
    g_geomIndex.emplace(key, id);
    return id;
}

int sbr_scene_split_triangles() { return g_splitTris; }

void sbr_scene_report_alpha() {
    if (g_alphaTotal == 0) return;
    lucent::info("nrender", "vertex alpha: {:.1f}% of {} textured vertices are below 0.5 "
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
    if (g_projThisTick) return;
    std::memcpy(g_proj, m, sizeof g_proj);
    static int once = 0;
    if (once < 2) {
        ++once;
        lucent::info("nrender", "projection rows: [{:.4f} {:.4f} {:.4f} {:.4f}] "
                                "[{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}] "
                                "[{:.4f} {:.4f} {:.4f} {:.4f}]",
                     m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                     m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
    g_projValid = true;
    g_projThisTick = true;
}
bool sbr_scene_has_projection() { return g_projValid; }
const float* sbr_scene_projection() { return g_projValid ? g_proj : nullptr; }

double sbr_scene_now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
int  sbr_scene_multislot_count() { return g_multislot; }

void sbr_scene_report_zmodes() {
    // Which depth states the scene actually uses. If this shows ONE state, per-material depth is
    // not what is occluding the plaza and the theory is wrong — cheaper to check than to assume.
    std::unordered_map<uint32_t, int> hist;
    for (const auto& d : g_cur.items) {
        const uint32_t k = (uint32_t)d.depth.test << 16 | (uint32_t)d.depth.func << 8 | d.depth.write;
        ++hist[k];
    }
    for (const auto& [k, n] : hist)
        lucent::info("nrender", "  zmode test={} func={} write={} -> {} drawables",
                     (k >> 16) & 1, (k >> 8) & 7, k & 1, n);

    // How many DISTINCT textures the tick's drawables reference. If this is 1, the binding is being
    // sampled at the wrong time (the FIFO is parsed once per frame, so reading it during the draws
    // returns last frame's final state for every shape) rather than per material.
    std::unordered_map<uint32_t, int> thist;
    for (const auto& d : g_cur.items) ++thist[d.tex[0].addr];
    lucent::info("nrender", "  {} distinct textures across {} drawables this tick",
                 thist.size(), g_cur.items.size());

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
                if (!d.tev.stage[s].texEnable) continue;
                maxMap   = std::max<unsigned>(maxMap, d.tev.stage[s].texmap);
                maxCoord = std::max<unsigned>(maxCoord, d.tev.stage[s].texcoord);
                // Which UNIT each enabled stage names, and whether that unit has a texture bound.
                // Binding four units scored WORSE than pinning every stage to unit 0, so the
                // question is whether the units above 0 actually carry the image the stage wants.
                ++mapHist[d.tev.stage[s].texmap];
                if (d.tev.stage[s].texmap < 4 && d.tex[d.tev.stage[s].texmap].addr == 0)
                    ++unboundStages;
            }
            if (maxMap > 0) ++multiMap;
            if (maxCoord > 0) ++multiCoord;
            if (d.tev.numTexGens > 1) ++multiGen;
            // The alpha test is only doing work where it is not ALWAYS with both comparisons.
            if (d.tev.alphaOp0 != 7 || d.tev.alphaOp1 != 7) ++alphaTested;
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
        for (const auto& [k, n] : mapHist)
            lucent::info("nrender", "    stage names texmap {} -> {} enabled stages", k, n);
        lucent::info("nrender", "    {} enabled stages name a unit with NO texture bound", unboundStages);
        // How many DISTINCT images each unit carries across the tick. Unit 0 varies per material
        // (83 distinct textures measured). If units 1-3 show only a handful, they are STALE —
        // holding whatever the last material to bind them left there — and a stage that names one
        // samples another material's texture. That is the difference between "bound" and "correct",
        // and it is the only thing separating the two bisect configurations.
        for (unsigned m = 0; m < 4; ++m) {
            std::unordered_map<uint32_t, int> uh;
            long lagSum = 0, lagMax = 0, n = 0;
            for (const auto& d : g_cur.items) {
                ++uh[d.tex[m].addr];
                // How many binds happened between this unit's last bind and the most recent bind
                // of ANY unit at capture time. Zero means the unit was bound as part of this
                // drawable's own material; a large lag means it still holds an earlier material's
                // texture, which is what "stale" actually means.
                uint32_t newest = 0;
                for (unsigned k = 0; k < 4; ++k) newest = std::max(newest, d.tex[k].bindSeq);
                if (newest == 0) continue;
                const long lag = (long)newest - (long)d.tex[m].bindSeq;
                lagSum += lag; lagMax = std::max(lagMax, lag); ++n;
            }
            lucent::info("nrender", "    unit {}: {} distinct addresses, bind lag mean {:.1f} max {}",
                         m, uh.size(), n ? (double)lagSum / (double)n : 0.0, lagMax);
        }
        lucent::info("nrender", "  texture units: {} drawables reference texmap>0, {} texcoord>0, "
                                "{} declare >1 texgen; {} use the alpha test (of {})",
                     multiMap, multiCoord, multiGen, alphaTested, g_cur.items.size());
    }

    // Blend state too: an unexpected factor pair washes the frame toward the clear colour just as
    // convincingly as a bad alpha, and the two are indistinguishable in the output.
    std::unordered_map<uint32_t, int> bhist;
    for (const auto& d : g_cur.items)
        ++bhist[(uint32_t)d.depth.blend << 8 | (uint32_t)d.depth.srcFac << 4 | d.depth.dstFac];
    for (const auto& [k, n] : bhist)
        lucent::info("nrender", "  blend mode={} src={} dst={} -> {} drawables",
                     (k >> 8) & 3, (k >> 4) & 7, k & 7, n);
}

void sbr_scene_report_largest(int n) {
    std::vector<Area> a = g_area;
    std::sort(a.begin(), a.end(), [](const Area& x, const Area& y) { return x.frac > y.frac; });
    for (int i = 0; i < n && i < (int)a.size(); ++i)
    {
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
        lucent::info("nrender", "  occluder #{}: shape 0x{:08x} el {} covers {:5.1f}%, {} verts, "
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

void sbr_scene_add(const SbrDrawable& d) {
    g_building.index.emplace(d.key, (uint32_t)g_building.items.size());
    g_building.items.push_back(d);
}

void sbr_scene_end_tick() {
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
        // Ignore absurd intervals (a load hitch, or the first tick after a stage change) rather than
        // letting one of them poison the period the interpolation runs at.
        if (dt > 0.002 && dt < 0.2) g_tickPeriod = g_tickPeriod * 0.9 + dt * 0.1;
    }

    g_lastCount = (int)g_cur.items.size();
    g_matched = 0;
    if (g_prev.valid)
        for (const auto& d : g_cur.items)
            if (g_prev.index.count(d.key)) ++g_matched;
}

float sbr_scene_render(double now_seconds, const float proj[16]) {
    if (!g_cur.valid) return 0.0f;

    // How far between the two snapshots this display frame falls. Derived from the WALL CLOCK, so
    // the render rate is independent of the tick rate — the whole point of the rework.
    float alpha = 1.0f;
    if (g_prev.valid && g_tickPeriod > 0.0) {
        alpha = (float)((now_seconds - g_cur.time) / g_tickPeriod);
        // Render one tick BEHIND: the scene is drawn between prev and cur, so it is always
        // interpolating between two KNOWN states rather than extrapolating past cur into a state the
        // game has not simulated. Extrapolation is what makes objects overshoot and snap back.
        alpha = std::clamp(alpha, 0.0f, 1.0f);
    }

    if (proj == nullptr) return alpha;   // nothing to place geometry with

    g_area.clear();
    for (const auto& d : g_cur.items) {
        if (d.geom == 0 || d.geom >= g_geom.size()) continue;   // no geometry decoded for this one
        const Geom& g = g_geom[d.geom];
        if (g.verts.empty()) continue;
        // SBR_RENDER_SKIP_SKINNED=1 — omit shapes whose vertices select more than one matrix slot.
        // Those are the shapes whose single per-drawable matrix is KNOWN to be wrong (the slot read
        // from a multi-matrix J3DShapeMtx is not a slot at all), so this isolates how much of the
        // picture their garbage transforms are responsible for.
        if (g_skipSkinned && g.multislot) continue;

        float m[12];
        const auto it = g_prev.valid ? g_prev.index.find(d.key) : g_prev.index.end();
        if (it != g_prev.index.end()) {
            lerp_mtx(m, g_prev.items[it->second].mtx, d.mtx, alpha);
        } else {
            // New this tick: no previous state to come from. Draw it AT its current transform rather
            // than interpolating from whatever memory happened to hold — the same provenance rule
            // that the old approach needed a special case for, here a natural consequence.
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
            const float vx = m[0] * v.x + m[1] * v.y + m[2]  * v.z + m[3];
            const float vy = m[4] * v.x + m[5] * v.y + m[6]  * v.z + m[7];
            const float vz = m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11];

            // The normal goes through the matrix's rotation only (no translation). J3D's draw
            // matrices are rigid plus uniform scale, so the 3x3 is its own inverse-transpose up to
            // a scale that renormalising removes.
            float nrm[3] = {m[0] * v.nx + m[1] * v.ny + m[2] * v.nz,
                            m[4] * v.nx + m[5] * v.ny + m[6] * v.nz,
                            m[8] * v.nx + m[9] * v.ny + m[10] * v.nz};
            const float nl = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
            if (nl > 1e-6f) { nrm[0] /= nl; nrm[1] /= nl; nrm[2] /= nl; }

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
            o.x = P[0]  * vx + P[1]  * vy + P[2]  * vz + P[3];
            o.y = P[4]  * vx + P[5]  * vy + P[6]  * vz + P[7];
            o.z = P[8]  * vx + P[9]  * vy + P[10] * vz + P[11];
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
                           (float)((v.rgba >> 8) & 0xFF) / 255.0f,
                           (float)(v.rgba & 0xFF) / 255.0f};
            const float vpos[3] = {vx, vy, vz};
            float lit[4];
            light_channel(d.xf, 0, vpos, nrm, vc, lit);
            const float vr = lit[0], vg = lit[1], vb = lit[2], va = lit[3];
            if (g_depthViz) {
                // Grayscale by normalised depth: near = black, far = white. If a surface that
                // should be distant paints itself dark (or vice versa) the transform for that mesh
                // is wrong, which per-object colours cannot show.
                const float dz = (o.w > 1e-4f) ? std::clamp(o.z / o.w, 0.0f, 1.0f) : 1.0f;
                o.r = o.g = o.b = dz;
                o.a = 1.0f;
            } else if (d.tex[0].addr != 0) {
                o.r = vr; o.g = vg; o.b = vb; o.a = va;
                ++g_alphaTotal;
                if (va < 0.5f) ++g_alphaLow;
                g_lumSum += (double)(0.2126f * vr + 0.7152f * vg + 0.0722f * vb);
                if (vr + vg + vb < 0.05f) ++g_lumBlack;
            } else {
                o.r = cr * vr; o.g = cg * vg; o.b = cb * vb; o.a = va;
            }
            // One final coordinate per texgen. Texgens the material does not declare keep zero
            // rather than the raw set, so an out-of-range texcoord selector shows as a corner
            // sample instead of quietly looking plausible.
            for (unsigned tg = 0; tg < 4; ++tg) {
                if (!g_texgenOn) {
                    o.uv[tg][0] = v.uv[0][0];
                    o.uv[tg][1] = v.uv[0][1];
                } else if (tg < d.tev.numTexGens) {
                    texgen(d.xf, tg, v, vc, o.uv[tg]);
                } else {
                    o.uv[tg][0] = o.uv[tg][1] = 0.0f;
                }
            }
            g_out.push_back(o);

            if (o.w > 1e-4f) {
                const float sx = o.x / o.w, sy = o.y / o.w;
                nlo[0] = std::min(nlo[0], sx); nhi[0] = std::max(nhi[0], sx);
                nlo[1] = std::min(nlo[1], sy); nhi[1] = std::max(nhi[1], sy);
                nearestW = std::min(nearestW, o.w);
            }
        }
        if (!g_out.empty()) sbr_render_tris(g_out.data(), (int)(g_out.size() / 3) * 3, d.depth, d.tex, d.tev);

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
    for (int i = 0; i < 3; ++i) { lo[i] = 1e30f; hi[i] = -1e30f; }
    std::vector<float> d;
    d.reserve(g_cur.items.size());
    for (const auto& it : g_cur.items) {
        const float t[3] = {it.mtx[3], it.mtx[7], it.mtx[11]};
        for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], t[i]); hi[i] = std::max(hi[i], t[i]); }
        d.push_back(std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]));
    }
    if (d.empty()) { *medianDist = 0.0f; return; }
    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
    *medianDist = d[d.size() / 2];
}

int sbr_scene_last_count() { return g_lastCount; }
int sbr_scene_matched_count() { return g_matched; }
