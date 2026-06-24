# 2026-06-24 — ★★★ ROOT CAUSE + FIX: ANK1 swapper underswapped the transform table by 3×

## The bug
`native/assets/anm_swap.cpp` `swap_ANK1()` byteswapped only `count * 0x12` bytes of the BCK's
transform key table, where `count = field_0xc` (the JOINT count from the header). But the
table contains **3 entries per joint** (one for each X/Y/Z axis — `J3DAnmTransformKey::
calcTransform` reads `mAnmTable[idx*3+{0,1,2}].mScale/Rotation/Translate`), so the real table
length is `count * 3 * 0x12` bytes.

For Mario's body BCK (29 joints): swapper swapped 29 entries (0x20A bytes), but the table
contains 87 entries (0x60E bytes). Entries 0..28 got the BE→host swap; entries 29..86 stayed
raw big-endian, so the eval read garbage u16 frame counts + offsets → indexed into garbage
within `mScaleData/mRotData/mTransData` → produced NaN-ish TRS for any joint whose lookup
landed in the unswapped region.

The first joint to read past the swapped region was joint 9 (idx*3 = 27, reaching entry 29 at
the exact boundary). From j9 onward, scales blew up (e.g. `scale.z = 3.99e12`), translates
diverged, and `MTXConcat` propagated the corruption through the skeleton recursion. At the
head joint (j28 / `M_head`), translation went NaN. `TMarioCap` planted that NaN base TR on
its cap models → the "TOO BA" rainbow shapes scattered at the bottom of file-select.

## The fix (one line, native-only)
```c
// before:
if (off_tab != 0)
    swap_run(out, off_tab, off_tab + count * 0x12 ... , 2);

// after:
if (off_tab != 0) {
    uint32_t end = off_tab + (uint32_t)count * 3u * 0x12u;
    if (end > size) end = size;
    swap_run(out, off_tab, end, 2);
}
```

## Verification (deterministic, no NaN fallback)
- Build: `cmake --build build-native --target sms-boot -j$(nproc)` clean.
- Run: `SB_STAGE=15 SB_SCENARIO=0 SB_SEL_DUMP=1 SB_SEL_DUMP_N=160 ./build-native/sms-boot`.
- Result: **160 frames dumped, zero NaN panics**, scene_verts climbed from ~2900 (pre-fix
  Mario-not-drawing) to ~4400 (Mario's skinned body now enters the draw buffer correctly).
- The "TOO BA" rainbow letters are GONE from the file-select frames. Mario's body now
  renders as a small figure on the beach (`scratch/frames/boot_0140.ppm`).

## Why this matters — global impact
The same underswap was hitting EVERY animated J3D model in the game, not just Mario:
- The trace pre-fix showed many other BCKs being loaded (`track_count=3, 29, 52, ...`).
- Any model whose BCK has track_count >= ~3 (i.e. any rigged character with multi-axis
  anim — Mario, NPCs, enemies, Yoshi, the spray, kept objects) would have had partial-table
  corruption from joint idx where `idx*3 + 2 >= count`. For low-count cases (3 tracks) ALL
  joints were corrupt past the very first.
- This was the silent-but-massive bug standing between sms-boot and ANY character animation.

## How it was diagnosed (in this session's order, end-to-end)
1. `M3UModel::perform(2)` `OSPanic`s on first NaN joint matrix (user-requested fail-fast). The
   panic dumps joint name, both the matrix and the persistent BMD TRS, plus a full joint walk.
2. Walk showed BMD TRS is CORRECT for every joint (scale=(1,1,1), reasonable BMD rotations).
3. Disproved hypotheses: sin/cos table init, BMD load, anim mtxCalc attachment.
4. `SB_J3D_CALC` per-joint trace of the `info` parameter in `J3DMtxCalcSoftimage::calcTransform`
   showed the *calc-time* info diverged from the persistent BMD TRS. j9 was the boundary
   where `info.scale.z` flipped to 3.99e12. `info_ptr` was a stack address (the anim eval
   path writes to a stack-local J3DTransformInfo) — confirming the divergence came from
   `J3DAnmTransform::getTransform()`, not from `joint->getTransformInfo()`.
5. Followed the data: `J3DMtxCalcAnm::calc` constructs `J3DTransformInfo info` on the stack
   and fills it via `mOne[0]->getTransform(idx, &info)`. Read `J3DAnmTransformKey::
   calcTransform` — eval uses `mAnmTable[idx * 3 + {0,1,2}]`.
6. `SB_ANM_DBG` logged the BCK loader's `track_count = field_0xc`. Mario's body BCK had
   `track_count = 29` (joint count), but the eval needs `count * 3 = 87` entries.
7. Cross-checked the ANK1 swapper: it multiplies by `0x12` once but not by 3 — caught the
   underswap. Fixed.

## Diagnostics retained (still useful for future J3D work)
- `SB_PANIC_FAIL` / `SB_ALLOW_NAN_JOINTS`: NaN-joint fail-fast in `M3UModel::perform(2)`
  (kept ON by default; if it ever fires again, it's a real bug — the panic names the joint
  and dumps the matrix + BMD TRS + a full skeleton walk).
- `SB_MARIO_DBG`: TMario perform + calcAnim baseMtx + TMarioCap pose/entry traces.

## Files touched
- `native/assets/anm_swap.cpp`: the actual fix (swap `count * 3 * 0x12` bytes).
- `reference/sms/src/M3DUtil/M3UModel.cpp`: NaN fail-fast (kept).
- `reference/sms/src/Player/MarioMain.cpp` / `MarioDraw.cpp` / `MarioCap.cpp`:
  `SB_MARIO_DBG` traces (kept).

## What's next for the file-select
Mario now renders as a tiny figure (he's positioned far from the camera on the beach rail in
the option scene). Verifying his pose looks correct is the next visual check. The residual
horizon dither band is the multi-layer-blend no-oracle trap (unrelated, deferred). After
Mario's pose is verified at file-select, the boot→file-select fidelity bar is met.
