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
2. **M2 — all SE: ✅ DONE (2026-06-12).** The JAI handle layer is native:
   - **TOuterParam port** on native tracks (vol multiply / pitch multiply / pan replace
     with weight `panPower[3]/32767`, applied before the parent combine — JASTrack.cpp:391).
   - **Per-sound move-param slots** (9× vol/pitch/pan, JAISound::initMoveParameter
     semantics) flushed to the worker's outer params once per JAI frame (60 Hz tick
     derived from the subframe clock), like sendSeAllParameter → setSePortParameter.
   - **Tees** (`overrides/se_native.cpp`, keyed by sound id read from guest JAISound+0x8):
     stopSoundHandle 0x80302224 (fade = vol slot 6 → 0 over N frames, then stop — matching
     `setSeInterVolume(6, 0, fade)`), JAISound::setVolume/setPan/setPitch
     0x8030a57c/a604/a68c, setSeCategoryVolume 0x803029a4 (`unk28[cat] = v/127`).
     startSoundBasic now also captures JAISoundInfo swbit/prio (gpr[9]).
   - **Lifecycle**: dispatch binds an ActiveSE{worker, id}; released when the worker's
     exported port2 goes busy→idle; same-id retrigger stops the old instance first unless
     swbit bit19 (JAISeEntry::storeBuffer semantics); pending requests expire after ~2 s.
   - Stop signal = `writePortImport(0, 0)` (looping snippets poll port0) + note-off all 8
     worker channels (envelope release, not hard cut).
   Verified (100 s headless boot→menus, `tools/audio/raw_profile.py` on the solo dump):
   jingle zcr 2476 (M1 unregressed), the game's own 30-frame jingle fade-on-skip is now
   audible, real category volumes captured (cat5=74 cat0=96…), 108 dispatches / 2163
   handle ops, zero unhandled BMS opcodes, zero missing waves, no voice/track leaks.
   NOT yet modeled (M2.5 candidates): 3D distance attenuation/pan from actor positions
   (the guest computes these into move-param slots we don't tee — only the JAISound
   setVolume/setPan/setPitch API surface is captured), fxmix/dolby outer params,
   per-category concurrent-sound limits and priority stealing.
3. **M3 — BGM: ✅ DONE (2026-06-12).** Sequenced music plays natively:
   - **BARC table** (mSound.aaf chunk 4, magic `BARC----`): 48 entries, 0x20 bytes each
     (name[14] @+0, u32 offset @+0x18, u32 size @+0x1C), offsets into sequence.arc
     (loaded whole). **BGM id & 0x3FF = BARC index** (0x80010010 → 16 k_title.com,
     0x8001000e → 14 t_select.com, 0x80010001 → 1 k_dolpic.com — all confirmed live).
     Entry 0 = se.scom, the init/SE BMS already running as the engine root.
     NOTE: sequence.arc has NO Vload header (JaiArcS.hed is not on the SMS FST) — the
     decomp's Vload path is dead code for SMS; BARC is the real seq table.
   - **Multi-root player**: the subframe driver ticks a list of root tracks (init/SE root
     + one per playing BGM); a finished/stopped BGM root is closed recursively and removed.
   - **Intake**: startSoundBasic tee routes seq-class ids (0x8xxxxxxx) → njas_bgm_start;
     queued via the same pending queue (engine may still be loading at title time). Same
     BGM already playing → skip (JAIBasic seq gate). Stream ids (0xCxxxxxxx) stay guest.
   - **Handles**: stop/fade/volume/pitch reuse the M2 registry (isBgm slots: no category
     volume, release when the root finishes; stop = recursive root close). Confirmed live:
     k_camera stopped with fade=20 on demo end, then k_dolpic started.
   - **Bank residency is moot natively**: all IBNK banks and all WSYS wave groups are
     decoded at load (merged wave-id tables), so wScene/ARAM residency is unnecessary.
     RISK (open): if two scene groups of one WSYS assign different waves to the SAME wave
     id, the merged table keeps the last group — no wrong-sounding instrument observed yet;
     revisit per-scene tables if a stage BGM sounds wrong.
   Verified (150 s headless autostart): title → file-select → camera demo → Delfino
   Plaza, 831 noteOns across multi-track roots (bank 0 progs), sustained music RMS
   5000–7000 / zcr 1500–3000 / d2e ≤0.21 (raw_profile.py), zero unhandled BMS opcodes,
   no voice/track exhaustion, no crash.
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
