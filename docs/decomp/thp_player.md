# THP movie player state machine (GMSE01 USA)

Decomp research notes for the port. The THP-transition NULL-deref (open bug)
and FMV pacing live here. Sources: `reference/sms/src/THPPlayer/*` (SMS ships a
*modified* Nintendo THPSimple library — see "SMS modifications") + USA map
(this library IS named in `reference/sms_gmse01_funcs.txt`) + spot disasm.

**Recompiled status:** all listed entry points are in `generated/functions.h`
(recompiled, none JIT-only) — including the decode core (THPVideoDecode
0x8036b644, THPAudioDecode 0x80372b84, the __THPDecompressiMCURow* family).
The historic FMV comb bug here was the no-op'd `dcbz` (fixed, 8bc12c5).

## Entry points (USA, named in map, VERIFIED-named)

| Addr | Function |
|---|---|
| 0x8001f6fc | `THPPlayerOpen(name, onMemory)` — DVD open + header/component parse |
| 0x8001f6a4 | `THPPlayerClose` (only when state==0) |
| 0x8001f5fc | `THPPlayerCalcNeedMemory` |
| 0x8001f3cc | `THPPlayerSetBuffer(u8*)` — partitions buffer (read bufs ×10 / texture sets ×3 / audio bufs ×3), starts audio-decode + read threads, waits prepare, state 0→1 |
| 0x8001f05c | `THPPlayerPrepare(frame, flag, audioTrack)` |
| 0x8001f000 | `THPPlayerPlay` — state 1/4 → 2 |
| 0x8001ef28 | `THPPlayerStop` — any state ≠0 → 0; cancels DVD + all threads, restores VI callback |
| 0x8001eee8 | `THPPlayerPause` — state 2 → 4 (internalState 4) |
| 0x8001ea34 | `THPPlayerDrawCurrentFrame(rmode, x, y, polygonW, polygonH)` — YUV→RGB TEV draw of dispTextureSet |
| 0x8001e920 | `THPPlayerDrawDone` — called every frame from TApplication::gameLoop (even outside movies) |
| 0x8001e994 / 0x8001e9a4 / 0x8001e9ec | GetState / GetAudioInfo / GetVideoInfo |
| 0x8001e608 | `THPPlayerSetVolume(vol, ramp)` |
| 0x8001de28 / 0x8001e00c / 0x8001e4f0 | THPGXYuv2RgbDraw / Setup / Restore |
| 0x80372b0c | `THPInit` (decoder core init) |
| 0x8036b644 | `THPVideoDecode(frame)` — JPEG-ish decode, runs on the video decode thread |
| 0x80372b84 | `THPAudioDecode` |

Static internals (PlayControl VI-retrace callback, ReadThread, VideoDecode
thread funcs, audioCallbackWithMSound, MixAudio, PopDecodedTextureSet…) are
unnamed locals below 0x8001de28 / between the named entries — UNRESOLVED
addresses; locate via the registration sites when needed (PlayControl is the
arg to VISetPostRetraceCallback inside SetBuffer per decomp source; the USA
disasm of SetBuffer's tail does NOT show that call — see "Contradictions").

## State machine (`ActivePlayer` struct, size 0x1D0, global BSS)

Struct (decomp, UNVERIFIED offsets — fileInfo 0x3C, THPHeader 0x30… compute
when needed): `open` BOOL, then `u8 state`, `u8 internalState`, `u8 playFlag`,
`u8 audioExist`, dvdError/videoError s32s, ring buffers at the tail.

`state` values (from code paths):

| state | Meaning |
|---|---|
| 0 | ERROR/stopped — Open/Close/SetBuffer/CalcNeedMemory only legal here |
| 1 | prepared (SetBuffer done, threads running, VI callback armed) |
| 2 | PLAYING (PlayControl pops decoded texture sets per retrace) |
| 3 | finished (last frame displayed & audio drained; set by PlayControl) |
| 4 | paused |
| 5 | error (dvdError/videoError observed by PlayControl) |

`internalState`: 0 idle, 2 playing-steady (gate for audioCallback mixing and
DrawCurrentFrame validity), 3 done, 4 paused, 5 error.

```
Open → state 0
SetBuffer → threads start, WaitUntilPrepare, state 1, VI post-retrace = PlayControl
Play (state 1|4) → state 2
  PlayControl (every VI retrace, ISR context):
    dvd/video error → state 5
    field-timing gates (ProperTimingForStart/NextFrame; interlaced honors
      VIGetNextField, progressive uses retrace*frameRate/(50|60)00 counting)
    pops decoded THPTextureSet (video ≤ audio+1 sync rule when audio exists)
    last frame+no audio left → state 3
Pause (state 2) → 4;  Play → 2
Stop (any ≠0) → 0: restore VI callback, DVDCancel, cancel read/video/audio
  threads, drain used-texture queue
```

Consumers: the game polls `THPPlayerGetState()` (TMovieDirector ends the movie
mode on state 3/5) and calls `THPPlayerDrawCurrentFrame` per frame.

## Threads & queues

- **Read thread** (only when !onMemory): DVD-streams frames into the 10
  THPReadBuffers (SMS plays movies from disc; boot/menus may use onMemory).
- **Video decode thread**: THPVideoDecode per frame → 3 THPTextureSet ring
  (Y/U/V planar buffers, DCInvalidate'd — *the dcbz/cache-correctness zone*).
- **Audio decode thread**: → 3 THPAudioBuffers.
- OSMessageQueues: PrepareReadyQueue, UsedTextureSetQueue (3 deep),
  decoded-set queue inside THPVideoDecode.c.

Port note: these are real guest OSThreads under our hybrid — they ran fine
since the paired-single/dcbz fixes, but any transition wedge investigation
should check all three threads' states plus the queues (the same lost-event
class as the CARD fix: a thread sleeping on an OSMessageQueue that only a
CoreTiming-scheduled completion would fill).

## SMS modifications vs vanilla THPSimple (important)

1. **Audio goes through JAS, not AI streaming.** `audioCallbackWithMSound`
   mixes THP PCM into `SoundBuffer[2][1120]` and is registered as a JASDriver
   mix callback (decomp comment `JASDriver::registerMixCallback(audioCallback,
   MixMode_InterLeave)`); MSound volume ducking is applied. **Consequence for
   native audio M4:** THP movie audio is audible today only because the guest
   JAS DAC path still runs; deleting the guest path must add a native tee for
   the THP mix callback or movies go silent.
2. `THPPlayerPause`/`Play` are called by TMarDirector around pause states
   (state 5/10/11/12, see mar_director_application.md) for the movie stage
   (`mCurrArea.stage == 1`).
3. `THPPlayerStop` is called from `nextStateInitialize(12)` and
   `currentStateFinalize` — i.e. inside scene transitions. **The open
   THP-transition NULL-deref most plausibly lives in this window**: Stop
   restores the VI callback and cancels threads while gameLoop still calls
   `THPPlayerDrawDone`/`DrawCurrentFrame` that frame, and `dispTextureSet`
   is NOT cleared by Stop (cleared only in SetBuffer) — a draw after Stop+
   buffer free dereferences a stale texture set. Unconfirmed — verify against
   the actual crash PC when that bug is next reproduced.

## Contradictions / dead ends

- Decomp `THPPlayerSetBuffer` ends with `OldVIPostCallback =
  VISetPostRetraceCallback(PlayControl)`; the USA disasm of the function's
  final block (0x8001f5a0..f5f8) shows only buffer-pointer stores and the
  return — the registration must happen earlier in the body or elsewhere in
  the SMS build (`VISetPostRetraceCallback` is not even in the USA name map).
  Treat the decomp's THPPlayer.c as *close but not line-accurate* for USA.
- `ProperTimingForGettingNextFrame`'s decomp has a known TODO (`curField`
  unused); do not lean on its exact progressive-timing math without disasm.
