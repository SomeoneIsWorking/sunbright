# 2026-07-21 — ASan memory-bug hunt (Delfino boot crash)

## Tooling: ASan works here, via clang

GCC's `libasan` is NOT installed on this box (and `valgrind` isn't either), but
**clang's ASan runtime works**. Build:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -Wno-everything" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -Wno-everything" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:print_stacktrace=1" ./build-asan/sms-boot/sms-boot
```
The whole project (incl. aurora/Dawn) compiles clean under clang. **Use this
before hand-debugging any suspected corruption** — it found four real bugs in one
session that hand-analysis had misattributed twice.

## ⚠️ Do not trust the unwinder on this codebase

The same crash was blamed on `TWaterGun::changeBackup` (eu-stack), then
`SDLModel::entryModelDataSDL` (lldb) — both wrong at the time. Instrumenting
`entryModelDataSDL` with an unconditional log showed **150 calls, every one with
valid arguments**, before the crash. Symbol attribution under -O2 here is
unreliable; **instrument and verify, don't trust frame #0.**

## Four real bugs found & fixed (each ASan-verified)

1. **Missing C++14 SIZED `operator delete`** (`JKRHeap.cpp`). `operator new` returns a
   pointer that sits INSIDE the malloc'd block (SbHostHdr immediately before it);
   only `sb_host_free_if_tagged()` can recover the base. We overrode the unsized
   delete but not `operator delete(void*, size_t)` — which the compiler emits
   whenever it knows the object size (std::string/std::vector buffers, most typed
   deletes). Those hit the default impl → libc `free()` on the offset pointer.
   `ERROR: attempting free on address which was not malloc()-ed ... 32 bytes inside`.
   Corrupted the host heap on essentially every std::string destruction.

2. **ODR violation in `sdk_stubs.cpp`** — it declared its own minimal
   `class JUTException` / `class JUTDirectPrint` (no data members, size 1) instead
   of including the real headers. `create()` returned a 1-byte object; callers
   (marerr.cpp) see the REAL class and wrote `mGamePad` at offset 0x68 through it.
   `global-buffer-overflow, WRITE of size 8` into neighbouring globals, every boot.
   Signatures had drifted too (`setPreUserCallback` void vs OSErrorHandler,
   `readPad` void vs bool). Fixed by including the real headers + correctly-sized
   zero-initialised storage.

3. **`Mtx` vs `Mtx44` for `C_MTXOrtho`** (`Application.cpp` gameLoop, 2 sites).
   `C_MTXOrtho` writes a full 4x4 (64 bytes); the locals were `Mtx` = f32[3][4]
   = 48 bytes. As arguments BOTH decay to `f32(*)[4]`, so the compiler cannot
   diagnose it → 16 bytes of stack smashed EVERY FRAME.
   `stack-buffer-overflow, WRITE of size 16, C_MTXOrtho <- gameLoop`.
   This is the exact class CLAUDE.md warns about ("4x4 write into a 3x4 buffer").

4. **DOUBLE byteswap of map.col counts** (`MapCollisionData::init`). Counts were
   read with typed `stream >> value` — and `JSUInputStream::operator>>(s32&)`
   already byteswaps — then `__builtin_bswap32()` was applied on top, putting them
   back into BE. `new TBGCheckData[unk1C]` then walked off the heap and died in the
   element ctor, caught as a write into our mmap allocator's PROT_NONE guard page
   (`SIGSEGV: invalid permissions for mapped object`). The stale comment justified
   the manual swap with "the raw read(&value,4) above copies bytes without
   swapping" — not what the code does; the comment described a failure its own fix
   caused. **Lesson: when a comment justifies a swap, re-check what the read
   actually is — operator>> must be the single swap point.**

## Where the Delfino crash stands

Still `exit 139` at `SB_STAGE=1`, but it now gets **further**: past collision
loading (fix 4), reaching gameplay. Current fault is a genuine NULL:

    SIGSEGV fault address=0x0
    frame #0: SDLModel::entryModelDataSDL(SDLModelData*, u32, u32) + 19
              (reads param_1->unk0, i.e. param_1 == nullptr)

`entryModelDataSDL` is only called from `SDLModel::SDLModel(SDLModelData*,u32,u32)`,
which is reached via `TMActorKeeper::createMActorFromAllBmd`. Next step: re-add the
NULL guard/log in `entryModelDataSDL` (it did not fire on the earlier binary because
the crash was elsewhere then) and symbolize the caller to find which actor hands it
a null SDLModelData.

Note the ASan build and the release build currently fault in different places
(ASan intercepts malloc, changing layout) — trust the release build for "where does
it actually die", and ASan for "what is genuinely invalid".
