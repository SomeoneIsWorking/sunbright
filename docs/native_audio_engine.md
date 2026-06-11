# Native audio engine — PC-owned audio, end to end (ratified 2026-06-11)

## Direction (user mandate)
Stop debugging the recompiled JAS/JAI stack. Port the audio engine to native PC code: the
game's recompiled logic only *requests* sounds (IDs, volumes, positions); everything below —
sequencing, voice allocation, sample decode, mixing, output — is host C++ reading data
straight from the ROM. No mails, no DSP, no audioproc thread, no guest heaps.

Why this is the right cut:
- Every audio failure to date was hybrid plumbing (IRQ delivery, mail phase, thread timing,
  intcount races, force_jit fallout) — never the data or the DSP math.
- The data side is PROVEN: tools/jingle (Yaz0→RARC→AAF→WSYS→AFC) renders the jingle from the
  ROM bit-perfectly by ear (docs/audio_data_formats.md).
- The hook surface is tiny and stable: MSound/JAI entry points the game already calls.

## Architecture

```
recomp game code                          native engine (runtime/native_jas/)
----------------                          ----------------------------------
startSoundSystemSE(id)  ── override ──▶   se_start(id)        BMS interpreter (SE)
MSBgm::startBGM(id)     ── override ──▶   bgm_start(id)       BMS interpreter (BGM)
stop/volume/pan/...     ── override ──▶   handle ops          voice params
                                          │
                                          ├─ data: loaded once at engine init from the ROM
                                          │  via DiscIO (mSound.aaf from nintendo.szs: WSYS,
                                          │  IBNK; Banks/*.aw; Seqs/sequence.arc BMS; .asn)
                                          ├─ sequencer: port of JASSeqParser/TTrack semantics
                                          │  (decomp reference/sms/src/JSystem/JAudio)
                                          ├─ synth: instruments (IBNK) → voices: AFC decode
                                          │  (port of tools/jingle/jingle.py, already correct),
                                          │  pitch from key/rate, ADSR envelopes, pan/dolby
                                          └─ mix → na_push_dsp (existing native sink — the
                                             device clock; servo/gate already built)
HardStream/THP audio: keep current path (it WORKS — it is what has been audible all along).
```

## Milestones
1. **M1 — jingle: ✅ DONE (2026-06-12, runtime/native_jas.cpp).** Native engine loads
   WSYS/IBNK/sequence.arc from the ROM at first SE, runs the REAL init/SE BMS (offset 0 of
   sequence.arc) through a TSeqParser/TTrack port, and synthesizes voices (IBNK keymap →
   WSYS wave → AFC PCM, TOscillator ADSR, C5BASE pitch). Intake = override on
   JAIBasic::startSoundBasic 0x803020ac (`runtime/overrides/se_native.cpp`): SE ids tee to
   `njas_se_start(id)`, original still runs (guest bookkeeping until M4). The engine does
   the exact JAIBasic port protocol: find idle worker track exporting `port9 == (id>>12)&0xFF`,
   write `port4 = id&0x3FF`, `port0 = 1`. Output mixes into the DSP stream inside
   `na_push_dsp` (`njas_mix`). Verified: engine-solo dump (`SUNBRIGHT_DUMP_NJAS=1` →
   scratch/wav/njas_solo.raw) jingle zcr/s 2475 vs reference w1stLoad_0_w02 2457 (0.7%),
   decay envelope matches window-for-window ×0.707 (center-pan stereo split). NOTE: the
   boot SE is id 0x7915 (not 0x7914 as previously assumed).
   **SE-BMS findings (tools/audio/bms_dis.py):** the init BMS opens per-category worker
   tracks (two mid tracks × ≤16 children); each worker loops `readport 0` until ==1, then
   `readport 4` → `call jumptable[port4*3]` into a per-sound snippet (e.g. cat-7 table at
   0xdbc, 304 entries; 0x7915 → snippet: r6=bank3/prog3, noteon key 60 vel 127, wait till
   note end). Snippets set r6 = (bank<<8)|prog and use simpleadsr/oscfull.
2. **M2 — all SE**: full SE BMS opcode coverage + category volumes + handles (stop/pan/pitch).
3. **M3 — BGM**: sequenced music (multi-track BMS, tempo, instruments across wScene banks,
   ARAM-equivalent bank residency = host RAM, no ARAM).
4. **M4 — delete the guest path**: audioproc thread never started; JAS DSP/ucode/driver/mails
   removed from the audio chain; Dolphin DSPHLE unused.

## Ground rules
- Reference implementation is the decomp (reference/sms/src/JSystem/JAudio) — port semantics,
  not emulate hardware. The oracle (DISABLE_RECOMP, with the pass-through guard) is the
  behavioral test: same scene, compare WAV pitch/envelope profiles.
- Data parsing in C++ mirrors tools/jingle/jingle.py exactly (it is the verified decoder).
- The native sink (native_audio.cpp) stays the device clock; the engine renders on demand
  from the sink's pull (true PC audio engine threading), not from emulated time.
- The recomp JAS stack stays in the binary but unreferenced once M4 lands; the
  seq-audio-dead wedge (CLAUDE.md frontier) becomes moot rather than fixed — note it as such.
