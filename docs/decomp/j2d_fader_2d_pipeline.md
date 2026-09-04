# The 2D / screenspace pipeline: J2DPane tree, J2DScreen, TSMSFader, Hx wiper (GMSE01 USA)

Decomp research notes for the port (widescreen overlays, fade/wipe pacing,
HUD work all live here). Unlike the MarDirector TU, the USA map
(`reference/sms_gmse01_funcs.txt`) names this subsystem well — addresses below
are from the USA map directly (VERIFIED-named) unless noted. Field offsets
marked VERIFIED were checked against `--disasm` of the USA binary.

**Binary evidence:** every function listed below was recovered from the analyzed GMSE01
executable. Remember override
blindness: recomp→recomp direct calls do NOT pass through overrides — a hook
on e.g. `J2DScreen::draw` only fires for dispatch entries unless the caller is
non-recomp or natively owned.

## Object model

```
J2DPane                      base 2D node, JSUTree-linked tree
 ├─ J2DScreen                tree root, loads .blo layouts ("SCRN"), drawn by owner
 │   └─ J2DSetScreen
 ├─ J2DPicture               textured quad (drawSelf / drawFullSet — the
 │                           widescreen HUD anchor point)
 ├─ J2DTextBox
 └─ (SMS user panes: TBoundPane 0x80155100, TBlendPane 0x8017910c, TExPane …
     created via J2DScreen::makeUserPane overrides)

JDrama::TViewObj             perform(u32 mask, TGraphics*) — everything 2D in
                             SMS is wrapped in a TViewObj (TGCConsole2, TGuide,
                             TTalk2D2, TPauseMenu2, TSMSFader…) and hangs in a
                             TViewObjPtrList ("Group 2D") performed by the
                             director's perform lists
```

### J2DPane field layout (VERIFIED: +0x14/24/34/44 JUTRects, +0xD0 JSUTree,
vtable 0x803e0598 — from `__ct__7J2DPaneFUsUlRC7JUTRect` 0x802cb0b0 disasm;
+0x84 mGlobalMtx independently verified by the HUD widescreen work, which
shifts m03 there)

| Off | Field |
|---|---|
| 0x00 | vtable |
| 0x04 | u16 mInfoTag ('PAN1' kind tags live in mKind) |
| 0x08 | u32 mKind |
| 0x0C | bool mVisible |
| 0x10 | u32 mUserInfoTag |
| 0x14 | JUTRect mBounds |
| 0x24 | JUTRect mGlobalBounds |
| 0x34 | JUTRect mClipRect |
| 0x44 | JUTRect mScissorBounds |
| 0x54 | Mtx mPositionMtx |
| 0x84 | Mtx mGlobalMtx (**m03 = row0 col3 X-translation — widescreen HUD shift point**) |
| 0xB4/0xB8 | int rotate offset X/Y |
| 0xBC | char mRotAxis ('x','y','z') |
| 0xC0 | f32 mRotation |
| 0xC4 | base position enum (9-way anchor) |
| 0xC8 | GXCullMode |
| 0xCC | u8 mAlpha; 0xCD u8 mColorAlpha; 0xCE bool influencedAlpha; 0xCF bool connected |
| 0xD0 | JSUTree<J2DPane> (parent/child/sibling links — tree walk root) |
| 0xEC | (J2DScreen) bool mbClipToParent; 0xF0 JUtility::TColor mColor |

### Key J2D functions (USA, all named in map, all recompiled)

| Addr | Function | Notes |
|---|---|---|
| 0x802cae18 | `J2DPane::__ct()` | |
| 0x802caf50 | `J2DPane::__ct(parent, u16, bool, u32, JUTRect&)` | |
| 0x802cb0b0 | `J2DPane::__ct(u16, u32, JUTRect&)` | VERIFIED disasm |
| 0x802cb1e4 | `J2DPane::__ct(parent, stream, bool)` | .blo deserialize |
| 0x802cfda8 | `J2DScreen::draw(x, y, J2DGrafContext*)` | the per-screen draw entry; ctx==null → builds its own `J2DOrthoGraph(0,0,640,480)` — **hardcoded 640x480 ortho**, which is why naive widescreen stretches 2D |
| 0x802d01c8 | `J2DScreen::drawSelf` | root background quad (mColor) |
| 0x802d0050 | `J2DScreen::search(u32 tag)` | find pane by 4cc tag |
| 0x802cc758/0x802cc7c0 | `J2DPicture::drawSelf(ii[,Mtx*])` | |
| 0x802cc838 | `J2DPicture::drawFullSet` | the quad emitter the HUD widescreen fix force-CFG'd and matrix-shifts |
| 0x802ccef4 | `J2DPicture::draw(iiiibbb)` | |
| 0x802cd2ec | `J2DPicture::drawTexCoord` | |
| 0x802eb460/0x802eb51c | `J2DGrafContext::__ct` | |
| 0x802eb6bc | `J2DGrafContext::setup2D` | ortho projection + 2D GX state |
| 0x802eb868 | `J2DGrafContext::setScissor` | |
| 0x802ecfcc/0x802ed0a8 | `J2DOrthoGraph::__ct` | |
| 0x802ed180 | `J2DOrthoGraph::setPort` | |
| 0x802cfd28/0x802cfd64 | `J2DScreen::makeUserPane(u16/u32,…)` | virtual — SMS subclasses build TBoundPane/TBlendPane/TExPane here |

Draw flow: `J2DScreen::draw` → (ortho port) → `J2DPane::draw` recursive tree
walk (visibility, alpha inheritance, makeMatrix → mGlobalMtx, clip/scissor)
→ per-pane virtual `drawSelf`. After the tree, draw() resets GX to PASSCLR /
no texgen / cull-none (a known GX-state side effect other code relies on).

## TSMSFader (the screen fader, TU GC2D/ScrnFader.cpp, USA 0x8013f638–0x801400cc)

Singleton-ish: `gpApplication.mFader` (TApplication+0x34); subclasses
TSmplFader (default ctor args black/60fps/"<ScrnFader>") and TShineFader
(shine-get white fade, `gpMarDirector+0xDC`).

Inherits JDrama::TViewObj (vtable+0, TNameRef name +0x4..0xB, TFlagT<u16>
unkC). Field layout — VERIFIED at +0x14 mRate, +0x1C unk1C, +0x20 mFadeStatus,
+0x24/0x28/0x2C mWipeRequest{type,time,delay}, +0x30 unk30, +0x34 unk34 (from
startWipe/requestWipe/update disasm):

| Off | Field | Meaning |
|---|---|---|
| 0x10 | u16 unk10 | fade duration in frames |
| 0x12 | u16 unk12 | fade frame counter |
| 0x14 | f32 mRate | frames/sec (SMSGetVSyncTimesPerSec at ctor; time args are SECONDS, converted via mRate) |
| 0x18 | TColor mFadeColor | fade overlay color (alpha animated) |
| 0x1C | bool unk1C | "resources ready" latch for bti-based wipes |
| 0x20 | EFadeStatus mFadeStatus | 0 FULLY_FADED_OUT, 1 FULLY_FADED_IN, 2 FADING_IN, 3 FADING_OUT |
| 0x24 | WipeRequest{u32 type; f32 fadeSecs; f32 delaySecs} | pending request; type 18 = none |
| 0x30 | int unk30 | ACTIVE wipe type (18 = idle) |
| 0x34 | f32 unk34 | active delay remnant |

### Wipe types (`startWipe` first arg / unk30)

From `requestWipe` + the Hx wiper API + MarDirector usage:

| Type | Meaning |
|---|---|
| 0–11 | Hx_* shaped wipes (Hx_StartWipe(type, frames)); fade direction from `Hx_GetWipeType(type)==1` → FADING_IN. Observed in MarDirector: 2 (scenario change), 6 (guide open), 8 (DOKAN/pipe wipe + SE), 10 (miss), 13 (game-over, loads gameover.bti), 14/15 used as plain fades below, 0xE used by save-quit |
| 12 | logo wipe — loads mmark.bti + logo.bti (UNK30_UNK_12) |
| 13 | game-over wipe — loads gameover.bti |
| 14 / 16 | plain fade-IN (`startFadein(frames)`) |
| 15 / 17 | plain fade-OUT (`startFadeout(frames)`) |
| 18 | none/idle |

### Function table (USA, all named in map, all recompiled)

| Addr | Function | Notes |
|---|---|---|
| 0x8013f638 | `setFadeStatus` | |
| 0x8013f680 | `setDisplaySize(int,int)` | called by TApplication::proc per mode with SMSGetGame/TitleRenderWidth/Height |
| 0x8013f6a8 | `load(JSUMemoryInputStream&)` | TViewObj virtual (scene file) |
| 0x8013f7c0 | `setColor(TColor)` | |
| 0x8013f808/0x8013f834 | `startFadeoutT/startFadeinT(f32 secs)` | |
| 0x8013f860 | `startWipe(u32 type, f32 fadeSecs, f32 delaySecs)` | VERIFIED disasm: 3 stores into mWipeRequest (+0x24/28/2C), nothing else — the wipe actually STARTS later, in update()→updateRequest() when delay hits 0. **This is the `ov_fader_startWipe` boot-pacing hook (`overrides/fader_pace.cpp`, sb_visual_live)** — note the hook fires at REQUEST time, delaySecs before anything is visible |
| 0x8013f870 | `requestWipe(WipeRequest*)` | VERIFIED disasm; dispatches to startFadein/out or Hx_StartWipe |
| 0x8013fa54 | `drawFadeinout(TRect&)` | plain alpha quad over given rect |
| 0x8013fc88 | `draw(TRect&)` | virtual; routes to drawFadeinout or drawWipe (Hx path); **rect comes from gameLoop = (0,0,fbWidth,efbHeight)** — EFB-sized, NOT 640x480: widescreen overlay coverage depends on the ortho projection set in gameLoop just before |
| 0x8013fe24 | `update()` | VERIFIED disasm; updateRequest (delay countdown at 1/mRate per call) then updateFadeinout alpha ramp (alpha = ±frames*255/(dur+1)) |
| 0x8013ff80 | `perform(u32, TGraphics*)` | TViewObj path (used when fader sits in a perform list, e.g. TShineFader) |
| 0x80140008 | `__ct(TColor, f32 rate, const char* name)` | |
| 0x801400cc | local `draw_wipe_box(TRect&, TColor)` | anonymous-namespace quad helper |

### Hx wiper (TU GC2D/hx_wiper, USA 0x8017df74–0x801820e0+)

C API, global state. `Hx_StartWipe(type,frames)` 0x80181fd8,
`Hx_UpdateWipe(f32)` 0x80181e80 (returns status; drives the shaped-wipe
animation), `Hx_GetWipeType` (inline/near), shape draws `Hx_Circle` 0x80181ab4,
`Hx_Door` 0x80180d80, `Hx_GameOver` 0x8018040c, `Hx_Test*` 0x8017df74–0x8017f520
(named test entries are the actual shape renderers), resource plumbing
`Hx_ProvideResourceEx` 0x801820e0 / `Hx_RemoveResource` 0x80182074. UNVERIFIED
beyond names — decomp has only the header for these (no .cpp); RE from binary
when the wipe shapes matter (they draw EFB-space quads/tris → same widescreen
concern as the fader).

## Who draws 2D when (call flow)

```
TApplication::gameLoop (0x802a5f50)               every frame, every mode
  mDirector->direct()                             ← mode content incl. HUD:
     TMarDirector::direct draws perform lists; the 2D ones
     (TGCConsole2/Talk2D/Guide/PauseMenu2, grouped under the "Group 2D"
     TViewObj, toggled via TFlagT bit 0xB in state changes) perform →
     each builds/uses a J2DOrthoGraph + J2DScreen::draw(0x802cfda8)
  GXSetViewport/Scissor(fb size) + C_MTXOrtho(0..fbW, 0..fbH)
  mFader->update(); mFader->draw(TRect(0,0,fbW,efbH))   ← fader is LAST,
                                                          over everything
  gpMSound->mainLoop()
```

Inside gameplay, the HUD console screens are drawn from TMarDirector's GX
perform lists during `direct()` (state-dependent masks — pause/demo masks
freeze movement but 2D perform lists unk38/3C/40 still run; see
`docs/decomp/mar_director_application.md`).

## Port-relevant notes

- **Widescreen 2D root cause** is structural: `J2DScreen::draw` with null ctx
  hardcodes 640x480 ortho; SMS code mostly passes its own J2DOrthoGraph built
  at 640x480 logical coords, then the EFB viewport stretches it. The HUD fix
  (drawFullSet mGlobalMtx m03 shift) re-anchors pictures; fade overlays and
  Hx wipes instead draw in fb-pixel space from gameLoop's ortho — different
  fix point (extend the TRect / ortho width, not pane matrices).
- `TSMSFader::startWipe` stores a request; the visible start is
  `delaySecs * mRate` frames later in `update()`. The boot-pacing override
  fires at request time — fine for pacing (GC-logo wipe has delay 0) but do
  not reuse it as a "wipe visible now" signal.
- Fade gates in scene transitions read `mFadeStatus` (+0x20): state 9/12 exit
  waits `isFullyFadedOut`, guide return waits `isFullyFadedIn`
  (changeState, MarDirector). Any native fader work must keep +0x20 truthful.
- mRate is captured from `SMSGetVSyncTimesPerSec()` — frame-rate coupled;
  if the port ever decouples vsync, fades change speed.

## Contradictions / dead ends

- decomp `ScrnFader.hpp` guesses `unk30` enum names (UNK30_UNK_12..18) — the
  binary confirms the *dispatch* (12/13 load resources; 14/16 fade-in, 15/17
  fade-out; default → Hx) but the decomp has no Hx implementation at all;
  shaped-wipe semantics are from the symbol names only.
- decomp J2DScreen header shows `draw(x, y, ctx)` copy-constructing the passed
  ctx; SMS USA disasm not checked instruction-by-instruction for this function
  body (J2D here closely matches pikmin2-era JSystem, which was cross-checked
  for J2DPane layout — identical offsets).
