# Super Mario Eclipse integration — assessed, deferred (not rejected)

**Question (user, 2026-08-04):** integrate Super Mario Eclipse into the port, "mostly because it has
native 60fps and widescreen".

**Answer: not now, and not for that reason.** Both of the stated motivations are already owned
natively here, and owned in a *better* form than Eclipse's. Integration remains interesting later,
as a platform milestone, and the technical shape it would take is recorded below so a future session
does not re-derive it.

---

## The stated reason does not survive contact with the tree

**Widescreen already exists, host-side.** `sms-recomp/overrides/widescreen.cpp`,
`widescreen_effects.cpp`, `docs/widescreen_effects.md`. We control the projection matrix directly,
which is strictly more capable than a guest-side patch — including the full-screen effect passes a
DOL patch typically leaves at 4:3.

**60fps is the active arc, and it is a different mechanism.** `sms-recomp/runtime/lerp60.cpp`,
`frame_smoothness.cpp`, design in `debug_journal/2026-07-30_aurora_60fps_lerp_design.md`. Ours is
*render* interpolation: the guest keeps ticking at 30 Hz and aurora emits two presents per tick, the
first with interpolated matrices. Game logic rate is untouched, so no actor needs a compat fix.

Eclipse's 60fps is the other approach — run guest logic at double rate and hand-fix everything that
assumed a 30 Hz tick. Adopting it would mean:

- inheriting its per-actor compat patches as a permanent maintenance surface, and
- **discarding the lerp work**, because the two are mutually exclusive: interpolating between ticks
  is meaningless once the tick *is* the display rate.

So the trade is "throw away in-flight work to acquire a maintenance burden that delivers the same
user-visible property". No.

## Why it cannot touch the decomp runtime at all

Eclipse ships as **compiled PPC**, injected into the retail DOL via the Kuribo module loader. There
is no source to hand-port into `decomp/sms`. The decomp+Aurora runtime is therefore not a candidate
under any plan — only the recomp is.

## Why the recomp cannot run it *today*

Static recompilation translates code ahead of time. Kuribo loads modules at runtime and **patches
instructions into live memory**. Self-modifying and dynamically-loaded PPC is precisely the class a
static recompiler cannot execute, and `sms-recomp/` has no interpreter fallback (checked: nothing
under `sms-recomp/` or `tools/recompiler/` provides one).

## The shape it WOULD take, when it is time

Recorded so this is a build, not a research question, when it comes up again:

1. **AOT-recompile the patched DOL as a second target.** Guest layout end-to-end, exactly the
   standalone-recomp model already blessed in CLAUDE.md. No new interop boundary — and specifically
   **not** the banned recomp↔decomp flip engine.
2. **Translate the Kuribo modules ahead of time too.** They are relocatable objects, so their code
   is statically enumerable; they are not the hard part.
3. **The hard part is the runtime patch calls.** Kuribo's "write instruction at address X" has to
   become a *fixup table applied at translation time* rather than a live memory write, since the
   target of the write is code we have already translated.
4. **Interpreter fallback for anything genuinely dynamic** that step 3 cannot resolve statically.
   This is the piece that does not exist and would have to be built.
5. **Assets user-supplied**, same rule as the base ROM — nothing Eclipse-derived enters the repo.

**Ordering: after the recomp runs retail end-to-end.** The value of the milestone is "this port is a
platform, not a one-ROM tech demo", and that claim is only meaningful once the one ROM works.

## Falsifiers — what would reopen this

- If Eclipse's 60fps turns out to be render-side rather than logic-rate (it is not, as understood
  here), the "mutually exclusive" argument collapses and the comparison must be redone.
- If the lerp60 arc is abandoned for an unrelated reason, the 60fps half of this note is void.
- If Eclipse ships a source distribution rather than DOL patches, the decomp runtime becomes a
  candidate and the whole assessment changes.
