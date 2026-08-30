# 2026-08-30 upstream rebase: animation/model API reconciliation

Before extending the decomp-side semantic renderer, `tools/re/rebase_upstream.py status` found seven
new `doldecomp/sms` commits. The required rebase replayed cleanly textually but the native Clang audit
failed. This was API replay damage, not a renderer failure.

## Root cause

Upstream renamed `MActor` model and animation members while Sunbright had independently named
`MActorAnmData` accessors and retained native-only SDLModel diagnostics and layout fixes. The replay
strategy combined old member references with new declarations inside the same translation units.
It also partially replayed the MarioCap field rename, leaving callers and the class declaration on
opposite vocabularies. The resulting files were internally inconsistent even though Git reported no
text conflict.

## Resolution

- Reconciled `MActor.hpp`/`MActor.cpp` as one unit around upstream's named actor fields, then applied
  the existing typed animation-data accessors and native `matanm` logger calls to those names.
- Kept the paired native `SDLModel.hpp`/`SDLModel.cpp` implementation because it contains the loud
  null-model refusal and host-layout adaptations that an upstream replacement would drop.
- Reconciled MarioCap and every reached direct `MActor::unk4` consumer onto the named model API.
- Named perform bit `0x10000000` as `CUE_UPDATE_MODEL_EFFECTS` from its observed cap-tremble owner.
- Ran the convergence tool on ten header/source units. Nine could not compile against the native
  tree. The sole green candidate only removed the documented gauge-body boundary, so it was kept
  and marked with `SMS_NATIVE_PLATFORM` for future convergence classification.

## Verification

- `tools/re/rebase_upstream.py audit`: native Clang build green.
- `./run.sh --diagnostic --runtime decomp --stage 15 --quit-after 100`: guarded title run exited 0.
- `tools/re/rebase_upstream.py converge --limit 10`: its independent guarded stage-1 gameplay smoke
  reached `APP_STATE_GAMEPLAY`, completed draw work, and passed.

These controls prove the synchronized tree still builds and reaches representative decomp frames.
They do not prove that the nine rejected convergence units are wrong upstream or that all remaining
decomp behavior is complete.
