# docs/ — what is where

Start at **`codemap.md`**. It is the one-page map of every subsystem: where it lives, whether it
works, and what is missing. Consult it at the start of a task and update it in the same commit that
changes a subsystem.

| | |
|---|---|
| **`codemap.md`** | **START HERE.** Every subsystem, its status, its gap |
| `architecture.md` | how the two runtimes fit together |
| `diagnostics.md` | GENERATED (`tools/diag_registry.py write`) — every diagnostic switch and what reads it. Do not hand-edit |
| `DO_NOT_REVISIT_FLIP.md` | a settled question. Read before proposing recomp↔decomp interop |
| `60fps/` | interpolated 60fps: the map of the paths, the effect work, screen effects |
| `audio/` | the JAS mixer port, data formats, the A/B harness |
| `app/` | launcher-owned settings, renderer selection, frame-rate policy, and RmlUi behavior |
| `port/` | the porting roadmap, worklist, workflow, threading model, renderer plan |
| `re_notes/` | per-subject reverse-engineering notes, one file per mechanism |
| `info/` | the CLAIMS and INSTRUMENTS registries (`tools/../project-info`) — what was proven, and which tools can be trusted. Machine-maintained; use the CLI |
| `decomp/` | notes specific to the `decomp/sms` submodule |

Findings go to **`debug_journal/<date>_<topic>.md`**, not here — dead ends as well as wins. `docs/`
holds things that describe the CURRENT state; the journal holds what happened and when.

## Conventions this directory follows

- **No handoff briefs.** A document that begins "read this first to continue" is a lossy self-summary
  standing in for a conversation, and it goes stale the moment the code moves. Two of them lived here
  for two months describing files that had been deleted; their durable content is now in
  `60fps/effects.md` and they are gone.
- **No tombstones.** When something is retired, references to it are deleted rather than annotated
  as retired. Absence is the cleanest documentation.
- **A stale doc is worse than a missing one**, because it is trusted. If a later session supersedes
  something here, fix the file — do not append a correction below the wrong version.
