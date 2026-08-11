// populations.h — the audit labels. One id per game-side system that emits geometry.
//
// These are NOT interpolation identities and nothing pairs on them. They exist so the audit can
// report which SYSTEMS interpolate and which snap, because a single global "77.8% of draws carry an
// identity" cannot separate a correctly-snapping HUD from world geometry stuttering, and cannot
// name the system responsible for either.
//
// Id 0 is deliberately "unlabelled": any draw no seam claims lands there, and the report says so
// rather than distributing it. That bucket IS the edge of the audit's knowledge, and it must stay
// visible — an audit that silently attributes what it does not know is worse than one with a gap.

#pragma once

#include <intrinsics.h>

enum SbPopulation : u8 {
    SB_POP_UNLABELLED     = 0,
    SB_POP_J3D_SHAPE      = 1,   // J3DShape::draw — the bulk of world geometry
    SB_POP_SHADOW_VOLUME  = 2,   // TMBindShadowManager::drawShadowVolume
    SB_POP_SHADOW_SHINE   = 3,   // TModelWaterManager::drawShineShadowVolume, per sphere slice
    SB_POP_SHADOW_MODEL   = 4,   // SMS_DrawShape — the pass-4 / ship shadow shapes
    SB_POP_PARTICLE       = 5,   // JPA billboards
    // LABEL-ONLY populations. Labelling is independent of TAGGING: these seams give their draws no
    // cross-tick identity — some cannot have one — but naming them is what turns the audit's
    // "(unlabelled)" bucket from an unknown into a statement. A population that snaps and is NAMED
    // is a decision; the same draws unnamed are an oversight that looks identical in the totals.
    SB_POP_FLAG           = 6,   // TMapObjFlag::draw — deforming cloth, immediate mode
    SB_POP_WAVE           = 7,   // TMapObjWave::draw — the sea ripple grid, rebuilt per tick
    SB_POP_DRAW_CUBE      = 8,   // SMS_DrawCube — the shadow pass's alpha-restore cube
    SB_POP_TEXT           = 9,   // JUTResFont::drawChar_scale — glyphs
    SB_POP_J2D            = 10,  // J2DPicture — 2D panes
    SB_POP_WIRE           = 11,  // TMapWire::drawUpper/drawLower — the rope, deforming per tick
    SB_POP_MIRROR         = 12,  // TModelWaterManager::drawMirror — the water-mirror mask fans
    SB_POP_STRIPE         = 13,  // JPADrawExecStripe/StripeCross — a particle CHAIN as one strip
    SB_POP_CONEBEAM       = 14,  // TConeBeam::drawConeBeam — the light-shaft cone, rebuilt per tick
    SB_POP_ROPE           = 15,  // TSwingBoard::drawOneRope — the swinging platform's ropes
    SB_POP_GRASS          = 16,  // TMapObjGrassGroup::drawNear — swaying grass blades
    SB_POP_BRIDGE         = 17,  // THangingBridge::perform — the rope bridge's ropes, all of them
    SB_POP_COGWHEEL       = 18,  // TCogwheel::draw — Noki Bay's 天秤 scale, its beam built per tick
};

void sbr_gxfifo_draw_pop(u8 pop);
void sbr_pop_register_names();
