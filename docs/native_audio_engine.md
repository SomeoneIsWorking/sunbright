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
1. **M1 — jingle**: native SE engine subset: load WSYS/IBNK + w1stLoad bank, hook
   startSoundSystemSE, interpret enough of the SE BMS to play MSD_SE_MV_CHAO (0x7914) voices,
   mix into the sink. Success = headless WAV matches w1stLoad_0_w02 (zcr 1208–1316).
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
