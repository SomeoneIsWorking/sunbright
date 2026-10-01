# Documentation map

Sunbright's living authorities are deliberately small and distinct:

| Document | Question answered |
| --- | --- |
| `project-state.md` | What is verified, partial, or missing, and what is the current focus? |
| `project-goals.md` | Why does the project exist and what outcomes define completion? |
| `architecture.md` | How does the native/dynarec product fit together? |
| `port/migration.md` | In what order does the executor migration land and what gates it? |
| `codemap.md` | Which subsystem owns each responsibility and where does work go? |
| `issues/` | Which atomic bugs, tasks and missing features are still open? |
| `re_notes/` and `decomp/` | What has been recovered about exact GMSE01 behavior and layouts? |
| `60fps/`, `audio/`, `app/` | What is the detailed subsystem contract? |

The canonical portfolio migration contract lives in the shared `jit-common` repository. Local docs
refine it for Sunbright and must not reintroduce offline guest translation, a gameplay interpreter,
or a second shipping runtime. A fact worth keeping belongs in exactly one of the documents above;
history is kept in Git, not in a parallel narrative tree.
