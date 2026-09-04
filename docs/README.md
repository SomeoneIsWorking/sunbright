# Documentation map

Sunbright's living authorities are deliberately small and distinct:

| Document | Question answered |
| --- | --- |
| `project-state.md` | What is verified, partial, or missing, and what is the current focus? |
| `project-goals.md` | Why does the project exist and what outcomes define completion? |
| `architecture.md` | How does the native/dynarec product fit together? |
| `port/migration.md` | In what order does the executor migration land and what gates it? |
| `codemap.md` | Which subsystem owns each responsibility and where does work go? |
| `issues/` | What atomic bug, task, blocker, finding, or dead end is recorded? |
| `info/claims/` | Which falsifiable facts have evidence? |
| `info/instruments/` | Which diagnostic tools can be trusted and within what scope? |
| `re_notes/` and `decomp/` | What has been recovered about exact GMSE01 behavior and layouts? |
| `60fps/`, `audio/`, `app/`, `graphics/` | What is the detailed subsystem contract and evidence? |

The canonical portfolio migration contract lives in the shared `jit-common` repository. Local docs
refine it for Sunbright and must not reintroduce offline guest translation, a gameplay interpreter,
or a second shipping runtime.

Historical debugging narratives remain in `debug_journal/`; they are not current architecture or
status. A fact that still matters should be reachable through a current claim, issue, RE note, or
state item rather than copied into several plans.
