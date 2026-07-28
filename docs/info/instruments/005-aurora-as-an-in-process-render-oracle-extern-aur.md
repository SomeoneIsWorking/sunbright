---
id: I005
kind: instrument
status: trusted
created: 2026-07-28
---

## Instrument

aurora as an in-process render oracle (extern/aurora, same process, same guest memory)

## Validated by

Renders Delfino Plaza correctly from the same FIFO stream the native path consumes; its g_gxState is directly readable. CAVEAT: it is NOT an oracle for anything both paths read identically — it is handed the same g_ram_base + phys pointer, so a texture that is zero in guest memory is zero for aurora too.

## Known failure modes

(none recorded yet)
