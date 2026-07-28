#pragma once
// tev_eval — the GX TEV pipeline as plain, testable C++, and the REFERENCE definition of it.
//
// Why this exists. The pixel pipeline lived only as a GPU shader (shaders/geom.frag.glsl), which
// means it could not be asked a question: no unit test, no way to evaluate one material, no way to
// see why a surface came out black except by rendering a whole frame and inferring backwards. That
// is black-box debugging, and it produced two confidently-wrong findings in a single session.
//
// So the arithmetic lives here, in a form that can be:
//   - UNIT TESTED against values hand-derived from the SDK (tests/tev_eval_test.cpp), rather than
//     against another run of itself;
//   - TRACED — every stage's inputs, its computed colour and alpha, and which register it wrote —
//     so "this surface is black" becomes "stage 3 wrote 0 to CPREV because C2 was 0";
//   - evaluated on the CPU for a real drawable, with no frame, no GPU and no game running.
//
// geom.frag.glsl mirrors this file. Where they disagree, THIS is the definition and the shader is
// the bug: this one is the one with tests.
//
// GX evaluates, per stage, for colour and alpha independently:
//     out = (d + sign * ((1-c)*a + c*b) + bias) * scale        [optionally clamped to 0..1]
// except when the op is a COMPARE (GXSetTevColorOp writes bias == 3 for those), where it is:
//     out = d + (cmp(a, b) ? c : 0)
// with no bias and no scale — the scale field carries the comparison WIDTH instead. See tev_compare.

#include <cstdint>

#include "native_render.h"

// What a stage sampled and what the rasteriser handed it. Supplied by the caller so the evaluator
// has no opinion about texturing — that is what makes it testable.
struct SbrTevInputs {
    // Sampled texel per texture unit, RGBA in 0..1. Only the units stages actually name are read.
    float tex[8][4]{};
    // The rasterised colour channel (RASC/RASA).
    float ras[4]{1, 1, 1, 1};
};

// One stage's working, for the trace. Values are pre-clamp so a stage that saturated is visible.
struct SbrTevStageTrace {
    unsigned stage = 0;
    unsigned texmap = 0, texcoord = 0;
    bool texEnabled = false;
    float texel[4]{};       // what the stage sampled (zero when its texture is disabled)
    float konst[4]{};
    float cArg[4][3]{};     // a, b, c, d as resolved colour arguments
    float aArg[4]{};        // a, b, c, d as resolved alpha arguments
    bool  cCompare = false, aCompare = false;
    float cOut[3]{}, aOut = 0;
    unsigned cDest = 0, aDest = 0;
};

// The whole evaluation, for the trace.
struct SbrTevTrace {
    unsigned numStages = 0;
    SbrTevStageTrace stage[16];
    float reg[4][4]{};      // the four registers after the last stage
    float out[4]{};         // the final colour, after the clamp
    int   alpha8 = 0;       // the quantised alpha the alpha test compares
    bool  alphaPass = true;
};

// Evaluate the TEV chain. `trace` may be null. Returns the final RGBA (pre-blend), and reports
// through `*discarded` whether the alpha test rejected the fragment.
void sbr_tev_evaluate(const SbrTevState& tev, const SbrTevInputs& in, float out[4], bool* discarded,
                      SbrTevTrace* trace);

// GX's TEV comparison, exposed for testing. `width` is the value the SCALE field carries for a
// compare op — 0 R8, 1 GR16, 2 BGR24, 3 RGB8/A8 — and `isEq` is the subtract bit, selecting == over
// >. Comparisons are on the 8-bit quantised values, packed little-endian across channels.
bool sbr_tev_compare_packed(unsigned width, bool isEq, const float a[3], const float b[3]);

// Render the trace as human-readable lines through the given lucent channel.
void sbr_tev_trace_report(const SbrTevTrace& t, const char* channel);
