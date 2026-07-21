# 2026-07-21 — Game-wide stage survey (first breadth measurement)

Prompted by feedback that the work had tunnel-visioned onto one narrow question
(NPC visibility at the Delfino spawn frame) while the actual goal is the WHOLE
game playable. This is the first measurement of where the game actually stands
across stages rather than inside one scene.

## Method

```bash
for s in 1..10; do
  SB_HEADLESS=1 SB_STAGE=$s SB_WATCHDOG_SECS=22 \
  SB_DUMP_FRAME=... SB_DUMP_FRAME_AFTER=150 timeout -s TERM 40 ./run.sh
done
```
`SB_GENOBJ_SKIP_ALL=1` turns the genObject fail-fast into a survey that lists
every missing type instead of panicking on the first one — use it to enumerate a
stage's gaps in ONE run.

## Result: 1 of 10 stages is healthy

| stage | exit | reaches gameplay | renders | note |
|-------|------|------------------|---------|------|
| 1 Delfino | 124 (alive) | yes | YES (145,151,156) | the only healthy stage |
| 2 | 134 panic | yes | no | missing actor types (8) |
| 3 | 139 segv | yes | no | **0 missing types** — other cause |
| 4 | 139 segv | yes | no | **0 missing types** — other cause |
| 5 | 134 panic | yes | no | missing ButterflyManager |
| 6 | 134 panic | yes | no | missing BossMantaManager (3) |
| 7 | 139 segv | yes | no | **0 missing types** — other cause |
| 8 | 134 panic | yes | no | missing FireWanwan/BeeHive (2) |
| 9 | 139 segv | yes | no | missing types (7) |
| 10 | 139 segv | yes | no | **0 missing types** — other cause |

Every stage REACHES gameplay; they die during scene population or first frames.

## Two distinct blocker classes

1. **Unregistered / unimplemented actor types** (stages 2, 5, 6, 8, 9). genObject
   hard-panics on the first unknown type, so each stage exposes its gaps one at a
   time — fixing one advances the panic to the next. Game-wide union of missing
   types (stages 1-10):
   `MareEventBumpyWall` (27 sites!), `ButterflyManager`, `YumboManager`,
   `WireTrapManager`, `TabePukuManager`, `SamboHeadManager`, `SamboFlowerManager`,
   `PakkunManager`, `MareEventWallRock`, `MapEventSirenaSink`, `KBossPakkunManager`,
   `IgaigaManager`, `GorogoroManager`, `FireWanwanManager`, `CannonManager`,
   `BossMantaManager`, `BossManta`, `BombHeiManager`, `BeeHiveManager`,
   `AmenboManager`.
2. **Non-actor crashes** (stages 3, 4, 7, 10) — these have ZERO missing types and
   still segfault, so they need their own diagnosis. Do NOT assume the actor
   burn-down fixes them.

## Landed this pass

`TFireWanwanManager` and `TAmenboManager` were fully implemented in-tree and only
missing their factory case — registered. Stage 8 now advances past FireWanwan to
BeeHiveManager (panic moved forward = real progress), stage 1 unregressed.

`TButterfloidManager` is also implemented but was BACKED OUT: registering it fails
to LINK because its base framework `TRealoid` / `TRealoidActor` has no bodies
(loadDefault, perform, ctors, typeinfo all undefined). Butterfly, boid and fishoid
all sit behind that same Realoid/Boid framework — port it once and flip all three
on together. That framework is therefore a high-leverage target: it unblocks
several actor families at once.

## Lesson

Checking `grep -rl "TFooManager"` in the factory file gave FALSE POSITIVES — it
matched commented-out `// TODO:` cases. The panic message is the ground truth for
"is this type actually registered".

## Root causes for the non-actor stage crashes (added same day)

Diagnosed with `build-dbg` (RelWithDebInfo + `-fno-omit-frame-pointer`).

**Stages 3 and 4 share ONE cause** — `SIGSEGV in __strlen_avx2`, immediately after
a run of `J2DScreen::search MISSING pane tag ... -> region-tolerant dummy`
(`sg06..sg08`, `t_1..t_4`, `b_1`).

The chain:
* Callers cast the search result to a richer subclass, e.g.
  `unk528 = (J2DTextBox*)unkB0->search('tet2')` (GCConsole2.cpp:358).
* `J2DScreen::search` falls back to `getRegionTolerantDummyPane()`, which returns
  a **plain `J2DPane`** placement-new'd into a zeroed 0x200 buffer.
* The buffer is deliberately over-allocated so derived non-virtual accesses land
  in it (the comment even anticipates `(J2DTextBox*)p->setString`) — but it is
  **zero-filled**, so a J2DTextBox's string pointer reads as NULL and the first
  `strlen` on it segfaults.

So the tolerated sentinel is safe for *pane* callers and unsafe for *TextBox*
callers. This is the success-shaped-stub hazard: it masks the missing pane right
up until someone treats it as the subclass it pretends to be.

⚠️ DO NOT "fix" this casually: **stage 1 — the only healthy stage — takes this
same dummy path** (its log shows pane misses too). Any change to the dummy must be
re-verified against stage 1 rendering, not just stages 3/4. A promising direction
is to make the dummy an actual `J2DTextBox` constructed with an empty string (it
IS-A J2DPane, so pane callers keep working and TextBox callers get a valid empty
string) — but the J2DTextBox ctors take a font/ResFONT, so that needs checking
before it is trusted.

**Stage 7** — jumps to address 0x0 (`frame #0: 0x0000000000000000`), i.e. a call
through a null function pointer / unpopulated vtable slot. Different cause.

**Stage 10** — `SIGSEGV fault=0x1` in `DVDClose(fileInfo=...)` at dvd.cpp:863.
Different cause again (bad/uninitialised DVD file handle).

lldb cannot unwind out of `__strlen_avx2` (hand-written asm, no frame pointer), so
frame #0 is all you get there — identify the caller from the surrounding log
context and the code, as done above.
