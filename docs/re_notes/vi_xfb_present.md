# RE: VI / XFB / Present pipeline (the 60 fps in-between-field path)

Reverse-engineered from the vendored SMS decomp at `reference/sms/`. Addresses from
`reference/sms_gmse01_funcs.txt`. This documents the path that the 60 fps interpolator
(`runtime/overrides/interp_redraw.cpp`) must drive, and the hazard that produces the
"each XFB shown twice" (RRBB) cadence instead of clean alternation (RBRB).

The EFB "screen texture" copy (`TEfbCtrlTex`) is documented separately; this note only
references it where it interacts with the scan-out path.

---

## 0. TL;DR / CONCLUSION (read this first)

- **SMS is SINGLE-XFB-BUFFERED.** `TApplication::initialize` allocates ONE 0xA5000-byte
  framebuffer and passes the SAME pointer as both XFB slots:
  `mDisplay = new JDrama::TDisplay(2, pvVar3, pvVar3, rmode)`
  (`reference/sms/src/System/Application.cpp:214,217`). So `TDisplay::unk4[0] == unk4[1]`,
  and the ctor sets the single-buffer flag `unk64.on(0x40)` (`JDRDisplay.cpp:22-23`).
  Every game frame: EFB → that one XFB → VI scans it out.
- **The game runs logic at 30 Hz and scans out 60 fields/s.** `TDisplay` is built with
  `param_1 = 2` → `unk4C (retrace count) = 2`. `endRendering` waits for **2 VI retraces**
  per call → 30 logic frames/s, 60 fields/s of the same image.
- **The present address is taken from the VI's "next frame buffer", which
  `TVideo::waitForRetrace` sets from `mNextFrameBuffer`** (the value handed to
  `TVideo::setNextXFB`). NOT from `TDisplay::unk4[]` directly. `endRendering`'s
  `IssueGXCopyDisp` writes the EFB into `unk4[unkC]` (the *copy* destination); the *present*
  address is whatever `setNextXFB` last latched. In single-buffer mode those coincide, which
  is exactly why a naive in-between copies to and presents from the same texture.
- **To get TWO distinct frames on screen per game tick**, the in-between must (a) copy the
  blended EFB to a **distinct XFB address** and (b) tell the VI to scan that distinct address
  out via `setNextXFB(alt)`, AND (c) make each present wait only **1** retrace (set
  `unk4C = 1` for the two sub-frames) so two presents fit in one 2-field game frame. This is
  what `interp_redraw.cpp` does (alt = `orig_fb ^ 0x00400000`).
- **The RRBB hazard:** if the in-between's copy or present lands on the *same* XFB address as
  the real frame (single-buffer aliasing), or if the present rate isn't split (still 2
  retraces each), the VI rescans one texture twice → the same image is presented twice
  (RR / BB) even though the cadence count looks like 60.

---

## 1. JDrama::TDisplay

Header: `reference/sms/include/JSystem/JDrama/JDRDisplay.hpp`
Impl:   `reference/sms/src/JSystem/JDrama/JDRDisplay.cpp`

### Layout (verified vs header offsets)

| Off   | Field                  | Meaning |
|-------|------------------------|---------|
| +0x00 | vtable                 | startRendering @+0 (`802f7fd8`), endRendering @+4 (`802f80d0`) |
| +0x04 | `void* unk4[2]`        | the TWO XFB buffer slots (in SMS, both == same pointer) |
| +0x0C | `u16 unkC`             | active buffer index, toggled `^1` each `endRendering` |
| +0x10 | `GXRenderModeObj unk10`| the render mode (fbWidth, efbHeight, xfbHeight, viTVmode, vfilter, aa, sample_pattern…) |
| +0x4C | `u16 unk4C`            | **retrace count** — fields to wait per frame (=2 → 30 fps) |
| +0x50 | `GXGamma unk50`        | display-copy gamma (GX_GM_1_0) |
| +0x54 | `GXFBClamp unk54`      | copy clamp (top|bottom) |
| +0x58 | `TColor mFrameBufferClearColor` | copy clear color (0,0,0,0) |
| +0x5C | `u32 mFrameBufferClearZ`| 0xffffff |
| +0x60 | `TVideo* unk60`        | the VI driver object (owns scan-out) |
| +0x64 | `TFlagT<u16> unk64`    | flags: 0x40 = single-buffer (set when buf0==buf1); 0x8/0x10/0x20 used by copy |

Note: the task brief said "TVideo ptr at +0x60" and "retrace count at +0x4C" — both
confirmed. (The brief's "two XFB pointers at +4/+8" is two slots inside the single
`unk4[2]` array at +0x04 and +0x08.)

### Construction (`JDRDisplay.cpp:9-25`)
```
TDisplay::TDisplay(u16 param_1 /*=2*/, void* param_2, void* param_3, const GXRenderModeObj&)
  : unkC(0), unk10(rmode), unk4C(param_1=2), unk50(GX_GM_1_0),
    unk54(CLAMP_TOP|CLAMP_BOTTOM), clearColor(0), clearZ(0xffffff), unk64(0x20)
{
  unk4[0] = param_2;             // == pvVar3
  unk4[1] = param_3;             // == pvVar3  (SAME pointer)
  if (param_2 == param_3) unk64.on(0x40);   // → single-buffer flag SET
  unk60 = new TVideo();
}
```
So `unk4C` defaults to **2** retraces; the single-buffer flag is always on in SMS.

### startRendering (`802f7fd8`, `JDRDisplay.cpp:27-34`)
```
unk60->setNextRenderMode(unk10);
unk60->setNextXFB(unk4[unkC]);        // tell VI which XFB to scan next = active slot
GXSetDispCopyGamma(unk50);
GXSetDispCopyFrame2Field(GX_COPY_PROGRESSIVE);
IssueGXPixelFormatSetting(unk10, unk64.check(0x8), unk64.check(0x10));
```
Important: `setNextXFB` is staged **here**, at the *start* of the frame, with the address of
the slot that was active *last* frame's draw. The actual VI program happens later in
`waitForRetrace`.

### endRendering (`802f80d0`, `JDRDisplay.cpp:36-47`) — the present/copy/wait core
```
unk60->waitForRetrace(unk4C);                 // (A) PACE: wait unk4C(=2) retraces + program VI
if (unk64.check(0x40)) {                       // (B) single-buffer always true → do the copy
    TRect rect(0,0, unk10.fbWidth, unk10.efbHeight);
    IssueGXCopyDisp(unk4[unkC], rect, unk10,   //     EFB → XFB[unkC] (the COPY destination)
                    mFrameBufferClearColor, mFrameBufferClearZ, unk54, unk64.get());
    GXFlush();
}
unkC = unkC ^ 1;                               // (C) toggle active slot (no-op for value;
                                               //     both slots equal in SMS, but flips index)
```
Ordering note: `waitForRetrace` runs **before** the EFB→XFB copy. By the time this frame's
`GXCopyDisp` writes the XFB, the VI has *already been told* (in this call's `waitForRetrace`)
to scan out `mNextFrameBuffer` — which was set by `startRendering`→`setNextXFB(unk4[unkC])` at
frame start. The pipeline is one frame deep: you draw into and copy to `unk4[unkC]`, and the
VI begins scanning that same buffer this frame.

How the retrace count governs fields-per-frame: `endRendering` calls
`waitForRetrace(unk4C=2)`, which blocks until 2 hardware retraces have elapsed since the last
frame and then arms the *next* deadline 2 retraces ahead (see §2). Two 60 Hz fields per
logic frame ⇒ **30 fps logic, 60 Hz scan-out of the same XFB**.

---

## 2. JDrama::TVideo — the VI driver

Header: `reference/sms/include/JSystem/JDrama/JDRVideo.hpp`
Impl:   `reference/sms/src/JSystem/JDrama/JDRVideo.cpp`

### Layout
| Off   | Field                          | Meaning |
|-------|--------------------------------|---------|
| +0x00 | `GXRenderModeObj mCurRenderMode` | currently programmed VI mode |
| +0x3C | `GXRenderModeObj mNextRenderMode`| staged mode (from `setNextRenderMode`) |
| +0x78 | `const void* mCurFrameBuffer`  | XFB currently being scanned out |
| +0x7C | `const void* mNextFrameBuffer`  | XFB to scan next (set by `setNextXFB`) |
| +0x80 | `s32 mLastRetraceTime`          | OSGetTick() of last frame (TimeRec) |
| +0x84 | `s32 mNextRetraceIndex`         | retrace-count deadline for the pacing wait |

### setNextXFB (`802fc99c`, `JDRVideo.cpp:29`)
```
void TVideo::setNextXFB(const void* fb) { mNextFrameBuffer = fb; }
```
A pure setter — it just records which address the *next* `waitForRetrace` will hand to
`VISetNextFrameBuffer`. **This is the present-address source.** Overriding only the EFB-copy
destination without also changing this leaves the VI presenting the original buffer (RE'd in
`interp_redraw.cpp:192-198`).

### waitForRetrace (`802fc9a4`, `JDRVideo.cpp:31-68`) — pace + program VI
```
while (mNextRetraceIndex - (int)VIGetRetraceCount() > 1)   // (1) PACE
    VIWaitForRetrace();

if (!IsEqualRenderModeVIParams(mCurRenderMode, mNextRenderMode)) {  // (2) mode change?
    VIConfigure(&mNextRenderMode);
    if (mCurRenderMode.viTVmode != mNextRenderMode.viTVmode) {
        VISetBlack(1); mCurFrameBuffer = 0; VIFlush(); VIWaitForRetrace();
        ... (NTSC↔PROG transitions wait 60 retraces) ...
    }
}

if (mCurFrameBuffer != mNextFrameBuffer) {                  // (3) PROGRAM SCAN-OUT ADDR
    if (mNextFrameBuffer != nullptr) {
        VISetNextFrameBuffer((void*)mNextFrameBuffer);       //   ← the present address
        VISetBlack(0);
    } else VISetBlack(1);
}

mCurRenderMode  = mNextRenderMode;
mCurFrameBuffer = mNextFrameBuffer;
VIFlush();                                                  // (4) latch shadow regs
VIWaitForRetrace();                                          // (5) wait one more retrace
mLastRetraceTime  = OSGetTick();
mNextRetraceIndex = param_1 + VIGetRetraceCount();          // (6) arm next deadline (param_1=unk4C)
```

Pacing math (steps 1, 5, 6): with `param_1 = unk4C = 2`, after the pacing `while` and the
single trailing `VIWaitForRetrace`, the call returns having consumed ~2 retraces and arms
`mNextRetraceIndex` 2 retraces into the future. Net: one `endRendering` per ~2 fields = 30 Hz.

**Crucial subtlety for the in-between (step 3):** `VISetNextFrameBuffer` is called ONLY when
`mCurFrameBuffer != mNextFrameBuffer`. In single-buffer SMS, `mNextFrameBuffer` is the same
constant every real frame, so after the first frame step (3) is usually a NO-OP and the VI
just keeps scanning the one buffer. An in-between that wants a *different* address scanned must
set `mNextFrameBuffer` to that distinct address so this inequality fires and
`VISetNextFrameBuffer(alt)` actually runs.

### Single vs double buffering — CONFIRMED SINGLE
`TApplication::initialize` (`Application.cpp:214-217`) allocates one buffer and passes it as
both args → `unk4[0] == unk4[1]`, `unk64` flag 0x40 set. There is exactly one XFB. The
`unk4[2]` array and the `unkC ^ 1` toggle are vestigial double-buffer machinery that operate
on two identical pointers.

---

## 3. The EFB → XFB copy

### IssueGXCopyDisp (`802f917c`, `JDREfbSetting.cpp:67-81`)
```
GXSetCopyClamp(framebuffer_clamp);                          // unk54 = TOP|BOTTOM
IssueGXSetCopyFilter(rm.aa, rm.sample_pattern, flags&0x20, rm.vfilter);  // deflicker
bool doClear = IssueGXSetCopyClear(clear_color, clear_z, flags);
GXSetDispCopySrc(0, 0, rm.fbWidth, rm.efbHeight);           // EFB source rect
u32 yscale = GXSetDispCopyYScale(GetRenderModeYScale(rm));  // efbHeight→xfbHeight scale
GXSetDispCopyDst(ALIGN_NEXT(rm.fbWidth,16), yscale);
GXCopyDisp(param_1 /*=unk4[unkC]*/, doClear);               // ← the actual EFB→XFB copy
```
- `GetRenderModeYScale` (`JDRRenderMode.cpp:47-56`) = `GXGetYScaleFactor(efbHeight, xfbHeight)`
  (no AA path in SMS — AA2x panics "future not implemented").
- The **deflicker copy filter** is the 7-tap vertical filter `rm.vfilter`, applied when
  `flags & 0x20` and `vfilter != nullptr` (`IssueGXSetCopyFilter`, `JDREfbSetting.cpp:39-44`).
  In `GXSetCopyFilter` (`GXFrameBuf.c:311-391`) the vfilter coefficients go to BP regs 0x53;
  with no vfilter the default 1:1 vertical weights are written. This is the anti-flicker blend
  of adjacent EFB lines into the XFB — it operates on the *source* EFB, not across XFBs.

### GXCopyDisp (`8035ecec`, `GXFrameBuf.c:399+`)
Writes the copy-source rect / size / stride BP regs, then the destination physical address:
`phyAddr = (u32)dest & 0x3FFFFFFF; phyAddr >> 5` → BP reg 0x4B. **This destination address is
`TDisplay::unk4[unkC]`** — the EFB-copy target. It is independent of the VI present address
(which comes from `setNextXFB`/`VISetNextFrameBuffer`). They alias only because SMS uses one
buffer.

### Relationship to the "screen texture" EFB copy (TEfbCtrlTex)
Distinct path (`JDREfbCtrl.cpp` calls `IssueGXCopyDisp(param_2->getFrameBuffer(), …)` into a
texture for in-engine reuse). It shares the same `IssueGXCopyDisp` plumbing but targets a
texture buffer, not the scan-out XFB. Documented separately — only relevant here in that an
interpolator that re-issues draw lists must not let the screen-texture copy clobber the
scan-out XFB.

---

## 4. Frame / retrace timing: 60 Hz fields vs 30 Hz logic

### The VI hardware retrace (`vi.c`)
- `retraceCount` (`vi.c:72`) is bumped once per field by `__VIRetraceHandler`
  (`vi.c:162-213`, `retraceCount += 1` at :194), then `OSWakeupThread(&retraceQueue)`.
- `VIWaitForRetrace` (`8034f684`, `vi.c:462-473`) sleeps on `retraceQueue` until
  `retraceCount` changes — i.e. blocks one field (~16.7 ms at 60 Hz NTSC).
- `VIGetRetraceCount` (`803504ec`, `vi.c:841`) returns `retraceCount`.
- **Register latching:** `VIFlush` (`803502e8`, `vi.c:793-809`) copies pending `regs[]`→
  `shdwRegs[]` and sets `flushFlag`. The retrace handler then calls `VISetRegs`
  (`vi.c:144-160`) which writes `shdwRegs[]`→`__VIRegs[]` **at the field boundary** (and only
  on the correct field parity). So a new scan-out address set via `VISetNextFrameBuffer`
  (`80350404`, `vi.c:811-825`, programs `tfbb/bfbb` via `setFbbRegs`) does not take effect
  until the next retrace latches it. This is the hard hardware constraint: **at most one new
  XFB address can be latched per field.**

### The 30/60 decision lives in two places
1. **Game logic rate:** `SMSGetVSyncTimesPerSec()` (`Application.cpp:76-90`) =
   `(NTSC?60:50) / 2.0f` ⇒ **30** (NTSC). `SMSGetAnmFrameRate = 60/30 = 2.0` — animations
   advance 2 "60-units" per logic frame. Faders, directors, audio all pace off this 30 Hz.
2. **Scan-out rate:** the VI always runs 60 fields/s. The coupling is `TDisplay::unk4C = 2`
   passed to `waitForRetrace`, which makes each `endRendering` consume 2 fields. So one drawn
   image is held on screen for 2 fields = the game's native 30 fps.

The main loop (`TApplication::gameLoop`, `Application.cpp:593-666`): per iteration —
`startRendering()` → input/director logic (`mDirector->direct()`) → fader/sound →
`endRendering()`. One iteration = one 30 Hz logic frame = 2 VI fields.

---

## 5. CONCLUSION — the present path an interpolator must drive

To put **two distinct images on screen per 30 Hz game tick** (true 60 fps), the interpolator
must, within one `endRendering`/game frame, perform two presents at distinct addresses split
across the two fields. The required sequence (and what `interp_redraw.cpp` does):

**Real frame (unmodified game path), address = `orig_fb`:**
1. `startRendering` → `setNextXFB(orig_fb)`.
2. game draws into EFB.
3. `endRendering`: `waitForRetrace(unk4C)` → `GXCopyDisp(orig_fb)` (EFB→orig_fb) → VI scans
   `orig_fb`.

**In-between field, address = `alt = orig_fb ^ 0x00400000`** (distinct, 32-aligned, in MEM1):
4. Re-issue / blend the draw passes into the EFB (object-level interpolation).
5. Point the EFB-copy destination at `alt`: set `display+4` and `display+8` (`unk4[0/1]`) to
   `alt` so `endRendering`'s `IssueGXCopyDisp(unk4[unkC])` writes the blend into `texture[alt]`.
6. Tell the VI to present `alt`: `setNextXFB(alt)` — the present address comes from here, NOT
   from `unk4[]`. (Both are needed: copy dest = where the pixels land; setNextXFB = what the VI
   scans. They must match.)
7. Make BOTH the real and in-between present at **1** field each: write `unk4C` (display+0x4C)
   = **1** for the two sub-frames so each `waitForRetrace(1)` consumes one field, and two
   presents fit in the one 2-field game frame. Restore the game's `unk4C` (=2) afterward.
8. Restore `display+4/+8` to `orig_fb` after the in-between so the next real frame copies to
   the canonical buffer.

Net cadence target: **R(orig) B(alt) R(orig) B(alt)…** — each field latches a different XFB,
each held one field.

### Hazards that cause the SAME image to be presented twice (RRBB)

- **H1 — single-buffer aliasing (the original 30 fps bug).** If the in-between copies to and
  presents from `orig_fb` (because SMS is single-buffer and we didn't redirect both the copy
  dest and `setNextXFB`), `GXCopyDisp` overwrites `texture[orig_fb]` and the VI rescans the
  same texture twice ⇒ RR. Dolphin keeps the XFB as a VRAM texture keyed by address
  (`bSkipXFBCopyToRam`), so writing the same address twice = one texture shown twice. FIX:
  distinct `alt` for the in-between (steps 5-6).

- **H2 — present-address source confusion.** Redirecting only the EFB-copy destination
  (`unk4[]`) but not `setNextXFB`/`mNextFrameBuffer` leaves `VISetNextFrameBuffer` pointing at
  `orig_fb`; the blend lands in `texture[alt]` but the VI still scans `orig_fb` ⇒ the
  in-between is invisible and `orig_fb` shows twice (RR). FIX: also `setNextXFB(alt)`.

- **H3 — `waitForRetrace` no-op skip of VISetNextFrameBuffer.** Step (3) of `waitForRetrace`
  only calls `VISetNextFrameBuffer` when `mCurFrameBuffer != mNextFrameBuffer`. If the
  in-between's `mNextFrameBuffer` equals the currently-scanned buffer, no new address is
  latched ⇒ same image rescanned. The distinct `alt` makes the inequality fire each toggle.

- **H4 — un-split retrace count.** If `unk4C` stays at 2 for the sub-frames, each present
  waits 2 fields; two presents then span 4 fields = 2 game frames, so only every other
  in-between is actually distinct in time — the present count looks like 60 but adjacent fields
  duplicate. FIX: `unk4C = 1` for the two sub-frames (step 7).

- **H5 — field-parity / one-latch-per-field (hardware).** `VISetRegs` (`vi.c:144-160`) latches
  the new XFB address only at a field boundary and only on the matching parity in non-progressive
  modes. Two address changes issued within one field collapse to one latch ⇒ a dropped
  in-between. The two presents MUST straddle two separate retraces (guaranteed by H4's
  `unk4C=1` + `waitForRetrace`'s trailing `VIWaitForRetrace`). Residual present-cadence jitter
  (~4-5 ms stddev) traces here: VI field timing vs two GXCopyDisp/frame is not perfectly 1:1.

### Verification instrument
Ground truth is the actual presented-XFB-address ring (`interp_redraw.cpp` `/interp60` probe,
`runtime/probe_server.cpp`): if the last N presents alternate between `orig_fb` and `alt`,
two distinct frames reach the screen (60 fps); if one address repeats, the in-between isn't
being presented (one of H1-H5).

---

## Uncertainty / flags

- `GXRenderModeObj` inner field offsets (vfilter, sample_pattern, aa) used by `IssueGXCopyDisp`
  are read from the struct as named in the decomp; not byte-offset-verified here (not needed —
  the copy plumbing is faithful as called).
- The exact present-cadence jitter root cause (H5 residual) is observed, not fully pinned —
  flagged in the project notes as a follow-up.
- The vfilter/deflicker affects only intra-frame EFB→XFB blending; it does not blend across
  the two XFB addresses and is orthogonal to the alternation. An interpolator does its
  cross-frame blend in the EFB (re-issued draw passes), before the copy.
