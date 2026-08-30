# Upstream convergence: compilation accepted broken gameplay

## Scope

The required pre-port sync picked up one new `doldecomp/sms` commit. After the rebase and lighting
API reconciliation built successfully, `rebase_upstream.py converge` selected 135 local files whose
upstream versions also built. That result was not safe to land.

## Runtime falsifier

The bounded decomp launcher reached three different failures while candidate files were restored:

- `JUTGamePad::read` and `TMarioGamePad::read` again fell off non-void functions. GCC optimized the
  resulting undefined behavior into invalid control flow.
- Four `MarNameRefGen` factory files lost locally completed scene-object cases, producing unknown
  object failures while a stage was being constructed.
- `SMS_GetLightPerspectiveForEffectMtx` again passed a 3x4 host stack object to a function that
  writes a 4x4 projection matrix. That retail overflow is benign in the PPC call layout but
  corrupted saved host state.

Those seven replacements compiled but failed the stage-1 gameplay smoke. Restoring them let the
game reach Delfino Plaza and complete all 400 diagnostic frames.

## Unexercised-loss audit

That successful run was only a floor. Reviewing removed lines and the commits that introduced each
local behavior found six more replacements that the chosen stage did not falsify:

- the low not-found sentinel and ground-list selection in `MapCheck.cpp`;
- the reverse-engineered rolling-block root transform in `MapObjRailBlock.cpp`;
- the `[12][2]` NPC-parts array layout that prevents adjacent heap corruption;
- the floating `RAND_MAX + 1.0f` expression that avoids signed integer overflow;
- the `GXEnd` required by Aurora after a ROM-font immediate-mode draw;
- explicit signed-16 casts for original MWERKS audio-table bit patterns.

All thirteen retained files now carry `SUNBRIGHT-KEEP` beside the reason. The final staged sync keeps
those local implementations and adopts 122 upstream file versions whose changes are equivalent
endian-aware stream reads, API naming, declarations, or SDK cleanup.

## Tool correction

`rebase_upstream.py converge` now:

1. proves the existing tree reaches gameplay before changing a candidate;
2. builds each candidate group;
3. runs the bounded decomp gameplay smoke after every build-green group;
4. recursively bisects a runtime failure to the smallest header/source unit; and
5. excludes any file carrying `SUNBRIGHT-KEEP`, with positive and negative self-test controls.

The bounded smoke still cannot prove dormant behavior. Removed-line and introducing-commit review
remain mandatory before a convergence commit.

## Verification

- `uv run --frozen python tools/re/rebase_upstream.py --selftest`: passed crash, startup-hang,
  no-frame, known-good, marker-positive, and marker-negative controls.
- `uv run --frozen python tools/re/rebase_upstream.py audit`: Clang decomp build green.
- `./run.sh --diagnostic --runtime decomp --run-secs 30 -- SB_STAGE=1 SB_DRAW_STATS=1`: reached
  `APP_STATE_GAMEPLAY`, completed frames 0 through 399, exited at the 400-present cap, and the live
  GPU watcher reported exit 0.
