# Decomp rebase reconciliation: particle registry and known emitter APIs

The decomp checkout built from a rebased source set whose `Particles.hpp` no longer contained 33
effect identifiers still referenced by the corresponding upstream implementations. This was a
header/source generation mismatch, not 33 new game-port gaps. Restoring the upstream registry made
the pair internally consistent.

The same rebase exposed call sites still reaching JPABaseEmitter through layout names such as
`unk154`, `unk160`, `unk174`, `unk180`, and `mChildSpawnRate`, after the emitter API had acquired
semantic owners. Those sites now use `setGlobalScale`, `setGlobalTranslation`, `setRate`,
`setGlobalParticleScale`, `setGlobalPrmColor`, and `setGlobalAlpha`. This follows the decomp lane's
standing order: rebase first, expand only what remains absent, then rename unknowns whose behavior
is established.

`TPerformList::load` also had two competing filter reads after reconciliation: one endian-aware
`readU32` and one stale raw four-byte read. It now has one authoritative read, names the cue bits,
and routes optional NameRef misses through the tracked `performlist` log channel. Stale environment
gated `fprintf` diagnostics in the touched static-object code were deleted rather than preserved as
a second logging system.

Verification at decomp commit `e0a7849a`:

- `python3 tools/re/rebase_upstream.py audit`: green.
- Clang Release `sms-boot` build: green.
- top-level CTest: 33/33 passed.
- bounded decomp+Aurora runtime: reached Delfino rendering until the 60-second wall cap; the
  unfiltered amdgpu boot count remained 42 (delta 0).

The runtime also showed that `SBR_QUIT_AFTER` is a recomp-only present cap today; the decomp smoke
was bounded by `run-safe.sh`'s wall timeout. That is a launcher-contract gap, not evidence that the
decomp runtime hung.
