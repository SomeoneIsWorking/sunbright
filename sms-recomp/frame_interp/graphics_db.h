// graphics_db.h — THE GRAPHICS REGISTRY. Every kind of graphic the game draws, whether anyone has
// reverse-engineered it, and whether it interpolates — kept in a file that outlives the run.
//
// ── WHY A FILE AND NOT A LOG LINE ───────────────────────────────────────────────────────────────
//
// The interpolation audit (populations.h) already answers "which systems interpolate" for ONE run,
// for the eleven populations someone thought to label by hand. It cannot answer the two questions
// that actually govern this work:
//
//   * WHAT IS THERE THAT NOBODY HAS LOOKED AT? A per-run report shows what the run drew. A graphic
//     that only appears in Bianco Hills is absent from a plaza report, and absent reads exactly
//     like "does not exist". Only an accumulating file can distinguish "not drawn this run" from
//     "not a thing".
//   * DID SOMEONE DECIDE, OR DID NOBODY LOOK? "snaps" from a system whose snapping is correct (a
//     HUD) and "snaps" from a system nobody has examined are the same word in a report and
//     opposite facts. The registry carries a CURATED verdict beside the MEASURED one, so the
//     difference is recorded rather than re-derived every session.
//
// ── THE KEY IS THE EMITTER SITE, AND IT IS ASSIGNED AUTOMATICALLY ───────────────────────────────
//
// A row is keyed by the guest address that emitted the geometry (the return address at the GX
// waist — GXBegin for immediate mode, GXCallDisplayList for indexed, J3DShapeDraw::draw for J3D).
// That key is automatic, which is the whole point: a graphic nobody has ever labelled still gets a
// row the first frame it draws, with its guest symbol resolved. Detection is not a list someone
// maintains — it is a consequence of drawing.
//
// The eleven curated populations keep their hand-written identity; sites are allocated ids above
// them. Both live in the same table because the question "what graphics are there" has one answer,
// not two.
//
// ── WHAT IT CANNOT SEE, stated here so the file's silence is never read as coverage ──────────────
//
//   * A graphic that never draws in any run that was recorded is ABSENT. The registry is a census
//     of what has been observed, not of what exists in the game.
//   * The `lerp` verdict comes from aurora's audit, which only classifies draws when interpolation
//     is running (SBR_LERP60=1). Rows from a non-interpolated run are written `unmeasured`, never
//     `no` — those are different facts and one of them is a defect.
//   * Two graphics emitted from the SAME call site are one row. Sites that need splitting are the
//     ones a curated population exists for.
//   * The id space is a byte, so at most 240 distinct sites can be told apart in one run. Sites
//     past that are counted and reported as an overflow, never silently folded into row 0.

#pragma once

#include <intrinsics.h>

// The GX waist a site was discovered at. Recorded because it says what KIND of geometry the
// emitter produces, which decides what interpolating it would even mean: indexed geometry has a
// persistent vertex array and can be matrix-lerped, immediate-mode geometry is rebuilt every tick
// and needs the vertex path.
enum class SbGfxWaist : u8 {
    Indexed,      // GXCallDisplayList — display-list geometry
    Immediate,    // GXBegin — immediate-mode geometry, rebuilt per tick
    J3DShape,     // J3DShapeDraw::draw — J3D's own shape draw, attributed to ITS caller
};

bool sbr_gfxdb_enabled();

// Attribute the primitives emitted from here on to `guestAddr` instead of to the waist's own
// caller; 0 clears it. For the SDK's draw helpers (GXDrawCube, GXDrawSphere), whose caller IS the
// waist's caller and therefore tells you only that a cube was drawn, never by whom.
void sbr_gfxdb_attribute_to(u32 guestAddr);

// Allocate (or recall) the population id for an emitter site, and count this call. Returns 0 when
// the registry is off or the id space is exhausted — 0 being the audit's "(unlabelled)" row, which
// is the honest place for a draw whose emitter could not be given its own row.
u8 sbr_gfxdb_site(u32 guestAddr, SbGfxWaist waist);

// Fold this run's measurements into the on-disk registry and write it. Called periodically rather
// than at exit only: automated runs are killed with SIGKILL, and a registry that is only written by
// an orderly shutdown would be empty for exactly the runs that do the most drawing.
// Record that a site's primitives were claimed by a hand-written SEAM, so the site's row can say so.
//
// Without this a site that GAINS a seam freezes at whatever verdict it last measured on its own. The
// seam takes the label, the detector stops allocating the site a population, its row is never
// rewritten, and the file keeps asserting `camera-only` for geometry that has interpolated since.
// That is the confidently-wrong note this registry exists to avoid, and it is invisible: the row
// looks like every other measured row.
void sbr_gfxdb_note_claimed(u32 guestAddr, u8 byPop);

void sbr_gfxdb_flush();

// One-line summary with its denominators, for the run log.
void sbr_gfxdb_report();
