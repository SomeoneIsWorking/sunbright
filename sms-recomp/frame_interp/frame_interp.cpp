// frame_interp.cpp — the one entry point. See frame_interp.h, and docs/60fps/README.md for the map.
//
// This is deliberately THIN. It owns the mode, the step, the presentation-sync flag and the
// callback registry — the vocabulary — and delegates the mechanism to the file that implements it
// (stream_interp.cpp today). The value of the layer is that a system asking "am I on a presentation
// frame, and what step is it at" gets one answer from one place, instead of the situation this
// replaces: three implementations, each with its own enable switch, none aware of the others, and a
// caller that had to know which one was compiled in to know which question to ask.

#include "frame_interp.h"

#include "app/frame_rate.h"
#include "populations.h"
#include "stream_interp.h"

#include <lucent/log.h>

#include <cstdlib>
#include <vector>

// aurora's snap, declared rather than included: aurora's gfx headers are internal to the library
// and the recomp links it statically, so the symbol resolves directly. Same approach the rest of
// this directory uses for aurora's tag counters.
namespace aurora::gfx {
void snap_next_interpolation();
namespace interp {
void name_population(uint8_t pop, const char* name);
void report_audit();
void report_ortho_motion();
void report_vertex_interp();
} // namespace interp
} // namespace aurora::gfx

// Registered once so the audit reads as systems rather than numbers. Kept here, beside the one
// place that knows the whole subsystem, rather than each seam naming itself — a seam that forgot
// would produce a numbered row that looks like a different population.
void sbr_pop_register_names() {
    static bool done = false;
    if (done)
        return;
    done = true;
    aurora::gfx::interp::name_population(SB_POP_J3D_SHAPE, "J3D shape (world)");
    aurora::gfx::interp::name_population(SB_POP_SHADOW_VOLUME, "shadow volume");
    aurora::gfx::interp::name_population(SB_POP_SHADOW_SHINE, "shine shadow slice");
    aurora::gfx::interp::name_population(SB_POP_SHADOW_MODEL, "shadow model");
    aurora::gfx::interp::name_population(SB_POP_PARTICLE, "JPA particle");
    aurora::gfx::interp::name_population(SB_POP_FLAG, "flag (deforming)");
    aurora::gfx::interp::name_population(SB_POP_WAVE, "sea ripple (deforming)");
    aurora::gfx::interp::name_population(SB_POP_DRAW_CUBE, "shadow alpha cube");
    aurora::gfx::interp::name_population(SB_POP_TEXT, "text glyphs");
    aurora::gfx::interp::name_population(SB_POP_J2D, "J2D pane");
    aurora::gfx::interp::name_population(SB_POP_WIRE, "wire (deforming)");
    aurora::gfx::interp::name_population(SB_POP_MIRROR, "water mirror mask");
    aurora::gfx::interp::name_population(SB_POP_STRIPE, "particle stripe (chain)");
    aurora::gfx::interp::name_population(SB_POP_CONEBEAM, "cone beam (deforming)");
    aurora::gfx::interp::name_population(SB_POP_ROPE, "swing-board rope");
    aurora::gfx::interp::name_population(SB_POP_GRASS, "grass (deforming)");
    aurora::gfx::interp::name_population(SB_POP_BRIDGE, "hanging-bridge ropes");
    aurora::gfx::interp::name_population(SB_POP_COGWHEEL, "balance scale (deforming)");
    aurora::gfx::interp::name_population(SB_POP_WIPE, "screen wipe (deforming)");
    aurora::gfx::interp::name_population(SB_POP_TDL_QUAD, "TDL indexed quad (deforming)");
    aurora::gfx::interp::name_population(SB_POP_HUD_GAUGE, "HUD water gauge (animated 2D)");
}

namespace sb::frame_interp {
namespace {

struct Registration {
    Callback cb;
    void* user;
};

std::vector<Registration> g_callbacks;
uint64_t g_tickSeq = 0;
bool g_syncRequested = false;
float g_presentationAlpha = 0.5f;

// Every one of these is a DENOMINATOR. A report that says "0 callbacks fired" is worthless without
// "out of 0 registered over 12,000 ticks", because those two zeros have opposite causes.
unsigned long g_ticks = 0;
unsigned long g_presentedFrames = 0;
unsigned long g_registrations = 0;
unsigned long g_dispatched = 0;
unsigned long g_syncedTicks = 0;

Mode resolve_mode() {
    if (!sbr_lerp_enabled())
        return Mode::Off;
    const Mode resolved =
        sb::app::frame_rate::interpolation_matches_refresh() ? Mode::Unlimited : Mode::Capped;
    static Mode previous = Mode::Off;
    if (resolved != previous) {
        lucent::info("interp", "frame interpolation: {}",
                     resolved == Mode::Unlimited ? "MATCH REFRESH" : "CAPPED (60 FPS)");
        previous = resolved;
    }
    return resolved;
}

} // namespace

Mode mode() {
    return resolve_mode();
}
bool is_enabled() {
    return mode() != Mode::Off;
}

float interpolation_step() {
    if (!is_enabled())
        return 1.0f;
    // A synced tick must present EXACTLY, which is step 1.0 — not 0.5 with the replacement
    // suppressed, because those differ for anything the replacement does not cover.
    if (presentation_sync_active())
        return 1.0f;
    return g_presentationAlpha;
}

void request_presentation_sync() {
    if (!is_enabled())
        return;
    g_syncRequested = true;
    ++g_syncedTicks;
    // Forwarded immediately rather than latched and forwarded at the seam: the mechanism that
    // consumes it already latches per tick, and a second latch here would only add a way for the
    // two to disagree about which tick a cut belonged to.
    aurora::gfx::snap_next_interpolation();
}

bool presentation_sync_active() {
    return is_enabled() && g_syncRequested;
}

void add_interpolation_callback(Callback cb, void* user) {
    if (!is_enabled() || cb == nullptr)
        return;
    g_callbacks.push_back({cb, user});
    ++g_registrations;
}

void begin_sim_tick() {
    ++g_ticks;
    ++g_tickSeq;
    // Cleared every tick, which is what makes the registry self-maintaining: a system that stops
    // registering stops being called, and nothing in this file has to know the list of effects.
    g_callbacks.clear();
    g_syncRequested = false;
}

uint64_t sim_tick_seq() {
    return g_tickSeq;
}

void present_interpolated_frame(float alpha) {
    if (!is_enabled())
        return;
    g_presentationAlpha = alpha;
    ++g_presentedFrames;
    for (const Registration& r : g_callbacks) {
        r.cb(/*is_sim_frame=*/false, r.user);
        ++g_dispatched;
    }
}

void report() {
    if (!is_enabled())
        return;
    sbr_pop_register_names();
    aurora::gfx::interp::report_audit();
    aurora::gfx::interp::report_ortho_motion();
    aurora::gfx::interp::report_vertex_interp();
    lucent::info(
        "interp",
        "frame interpolation: {} simulation tick(s), {} in-between frame(s) presented, "
        "{} presentation-sync request(s). Callbacks: {} registration(s) produced {} "
        "dispatch(es).{}",
        g_ticks, g_presentedFrames, g_syncedTicks, g_registrations, g_dispatched,
        g_registrations == 0
            ? "   <-- NO SYSTEM REGISTERED A CALLBACK. That is not 'no effect needed one': "
              "it means nothing in this build opts into presentation-frame work, so every "
              "effect is running at the simulation rate inside an interpolated frame."
        : g_dispatched == 0
            ? "   <-- registrations happened but NOTHING WAS EVER DISPATCHED, so "
              "present_interpolated_frame() is not being reached. The registrations are "
              "being cleared by the next tick before the in-between frame presents."
            : "");
}

} // namespace sb::frame_interp
