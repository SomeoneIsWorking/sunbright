# 2026-07-15 — SMS_GetLightPerspectiveForEffectMtx: PORTED + verified

**STATUS: DONE.** Landed in `reference/sms/src/MarioUtil/MtxUtil.cpp`, stub removed. The
camera-field blocker was resolved: `gpCamera` is `CPolarSubCamera : JDrama::TLookAtCamera :
JDrama::TCamera` (JDRCamera.hpp), which has clean accessors — `getFovy()`(mFovy@0x48),
`getAspect()`(mAspect@0x4c), `getNear()`(mNear@0x28), `getFar()`(mFar@0x2c) — all matching the
disasm (the earlier conflict was the WRONG TCamera, Camera.hpp's game camera vs the JDrama one).

**Native-adaptation bug fixed (important, general class):** retail runs `C_MTXPerspective`
(a 4x4 = 64-byte write) directly into the caller's buffer + writes row 3, but the callers
declare a 3x4 `Mtx` (48 bytes; `setEffectMtx(Mtx)` consumes only 3x4). Retail overflows that
buffer by 16 bytes — harmless on the PPC stack, but a DETERMINISTIC SIGSEGV on native x86-64
(corrupted an adjacent perform's stack → TEnemyManager null list base). Fix: build the
projection in a local `Mtx44`, then write only the observable 3x4 (rows 0-1 from projection,
row 2 = the {0,0,-1,0} effect fixup); drop the vestigial never-read row 3. Same observable
result, no overflow. Verified: boot advances through the full perform-list (enemy managers
included) to the pre-existing storage-overflow point — the SIGSEGV is gone.

---
## (historical) RE writeup — 90% note, kept for the derivation

Stubbed in `sms-boot/boot_stubs/unresolved_stubs.cpp:112`
(`void SMS_GetLightPerspectiveForEffectMtx(MtxPtr)`). Declaration only in the decomp
(`MtxUtil.hpp:160`), no body; absent from `reference/sms_gmse01_funcs.txt` (funcs.txt gap).
Used as a **texture effect matrix** (projected texgen) via `J3DTexMtx::setEffectMtx` — e.g.
`TIceBlock::calc` (MapObjBlock.cpp:273), `TShimmer::perform` (Shimmer.cpp:62),
`TMapStaticObj`, NpcParts, telesa, namekuri.

## US address + disasm (found via caller TShimmer::perform 0x8019f83c → bl 0x8022ba74)

US **0x8022ba74**, size 0x6C. Cross-checks the region symbols (JP GMSJ01 0x800C6E58,
PAL GMSP01 0x802239C8, size 0x6C).

```
mr    r31, r3                 ; r31 = m (MtxPtr arg)
lwz   r4, -0x7118(r13)        ; r4 = gpCamera  (SDA1 0x804141c0-0x7118 = 0x8040d0a8) [CPolarSubCamera*]
lfs   f4, 0x2c(r4)            ; far
lfs   f3, 0x28(r4)            ; near
lfs   f2, 0x4c(r4)            ; aspect
lfs   f1, 0x48(r4)            ; fovY
bl    C_MTXPerspective        ; C_MTXPerspective((Mtx44)m, fovY, aspect, near, far)
; overwrite the z + w rows (m is a 4x4 — writes up to +0x3c):
m[2] = { 0, 0, -1, 0 }        ; +0x20,+0x24,+0x28(-1.0),+0x2c
m[3] = { 0, 0,  0, 1 }        ; +0x30,+0x34,+0x38,+0x3c(1.0)
```

SDA2 constants resolved (tools/re/dol_sda.py --sda2): -0x1710=0.0, -0x1700=-1.0, -0x170c=1.0.

## The shape of the port (ready — one blocker)

```cpp
void SMS_GetLightPerspectiveForEffectMtx(MtxPtr m) {
    C_MTXPerspective(reinterpret_cast<Mtx44Ptr>(m),
                     gpCamera-><fovY@0x48>, gpCamera-><aspect@0x4c>,
                     gpCamera-><near@0x28>, gpCamera-><far@0x2c>);
    m[2][0]=0; m[2][1]=0; m[2][2]=-1.0f; m[2][3]=0;
    m[3][0]=0; m[3][1]=0; m[3][2]=0;     m[3][3]=1.0f;
}
```
Home: `reference/sms/src/MarioUtil/MtxUtil.cpp` (globbed); delete the unresolved_stubs entry.
C_MTXPerspective is available (aurora `lib/dolphin/mtx/mtx.c`, decl `dolphin/mtx.h`).
`gpCamera` = `extern CPolarSubCamera* gpCamera` (Camera.hpp:474).

## BLOCKER (do this first next session): camera projection field NAMES

The disasm reads retail offsets cam+0x48(fovY)/0x4c(aspect)/0x28(near)/0x2c(far) as f32, but
on the NATIVE build the layout differs (LP64, compiler layout) — must use FIELD NAMES, not
raw offsets. The decomp header is unreliable here: `Camera.hpp` TCamera lists
`/* 0x28 */ f32 unk28` and `/* 0x2C */ s16 unk2C` — but the DOL reads f32 at 0x2c, so either
`unk2C` is mis-typed (should be f32 = far) or the projection params live on an intermediate
base (`CPolarSubCamera : JDrama::TLookAtCamera : … : TCamera`; no getFovy/getNear accessors
exist). RESOLVE: trace the JDrama camera hierarchy to the fovy/aspect/near/far fields and map
0x48/0x4c/0x28/0x2c to their native member names (or add correctly-typed members), then the
port + a spec test (compare against C_MTXPerspective with the same params + the z/w fixup)
lands cleanly. Pure math, no rendering — verifiable while the Delfino render path is
desync-blocked.

## Meta (frontier scouting result, 2026-07-15)

Every Delfino boot-stub candidate scouted this session needs non-trivial cold-RE:
TResetFruit::initMapObj (undeclared GXColorS10 @0x19c + its init path), the Mtx callbacks
(not in funcs.txt), this one (camera field map). No trivial quick ports remain; the loop
needs either the render-unblocking desync fix (makes draw-method ports verifiable) or
dedicated per-method RE. Recorded so the next session starts from here, not from scratch.
