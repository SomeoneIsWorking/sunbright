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
  ROM bit-perfectly by ear (docs/audio/data_formats.md).
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
                                          │  (decomp decomp/sms/src/JSystem/JAudio)
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
   JAIBasic::startSoundBasic 0x803020ac (`runtime/overrides/se_native.cpp` (DELETED)): SE ids tee to
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
   - **Tees** (`overrides/se_native.cpp` (DELETED), keyed by sound id read from guest JAISound+0x8):
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
   **M2.5 — 3D distance attenuation: ✅ DONE (2026-06-12).** The SE param tees moved one
   level down to the inner setters `setSeInterVolume` 0x8030b700 / `setSeInterPan`
   0x8030b8c8 / `setSeInterPitch` 0x8030be20 (this=r3, slot=r4, f1=value, time=r5) — the
   funnel for BOTH the public JAISound API and the per-frame guest distance code
   (MSHandle::setSeDistance* → setSeInter*(4, …)). Verified: ambient SEs (0x5004/0x5005)
   receive per-frame vol/pan/pitch (e.g. v=0.965 / pan 0.542 / pitch 0.95±random wobble,
   4-frame smoothing) — 23 k param events over a 130 s run. Outer setVolume/setPan/setPitch
   tees now route seq (BGM) ids only.
   Still NOT modeled: fxmix/dolby outer params, per-category concurrent-sound limits and
   priority stealing.
   **GOTCHA — worker dispatch must claim, not just read port2 (fixed 2026-06-12):** the
   worker's exported busy port only updates after its BMS ticks, so two requests in one
   pending pass both saw the worker "idle" and double-dispatched (second id overwrote
   port4; bindings mismatched; the guest's stop-by-id then released the wrong slot and
   looping ambient voices leaked until all 64 were exhausted — d2e roughened to 0.4).
   `find_worker` now also rejects workers with a bound ActiveSE (`worker_claimed`), and
   dispatch is skipped when the registry has no free slot (binding is mandatory).
   Verified: 0 out-of-voices over a 150 s gameplay run (was 8574), clean d2e ≤0.14.
   Diagnostics: out-of-voices now dumps the 64-voice table (owner/state/gate/loop/age)
   under SUNBRIGHT_DBG_NJAS.
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
   - **Wave-scene residency (fixed 2026-06-12 — was the Delfino breakdown):** wsys 2
     ("wScene") has 22 per-stage groups whose wave ids HEAVILY overlap (group 16 alone
     collides on 107 ids) — a merged table plays the wrong samples outside the
     last-parsed stage. Now each group keeps its OWN table; the active group per wsys is
     teed from the guest. TEE GOTCHA: `JAIBasic::loadGroupWave` is VIRTUAL and SMS
     overrides it (MSound::loadGroupWave — not in the symbol map; its wScene path loads
     .aw files itself via MSLoadWave, additive residency, never reaching WaveBankMgr) —
     the covering tee points are `JAIBasic::loadSceneWave` 0x803017b0 (stage loads) +
     `WaveBankMgr::loadWave` 0x80310994 (init/stay + the 0x210 common-group direct).
     Lookup order: active scene group → group 0 → first group that has the id (handles
     wsys-2 additive co-residency, e.g. Delfino scene 1 + common group 16).
   - **Voice stealing (fixed 2026-06-12):** dense sequences (k_dolpic's mandolin tremolo,
     ~20 notes/s/track) legitimately spawn notes faster than long looping-wave releases
     decay; the 64-voice pool saturates by CHURN and rejecting new notes mutes the
     melody to keep release tails (hardware reclaims via DSP-channel priority stealing/
     breakLower). On exhaustion the allocator now steals the quietest RELEASING voice
     (envelope states 4/5/6, lowest phase). 0 out-of-voices over 150 s incl. Delfino.
   Verified (150 s headless autostart): title → file-select → camera demo → Delfino
   Plaza, 831 noteOns across multi-track roots (bank 0 progs), sustained music RMS
   5000–7000 / zcr 1500–3000 / d2e ≤0.21 (raw_profile.py), zero unhandled BMS opcodes,
   no voice/track exhaustion, no crash.
   **M2.5 CORRECTION (2026-06-12): the "inner setters are the funnel" claim is FALSE for
   SMS distance code.** The compiler INLINED setSeInterVolume into
   JAISound::setSeDistanceVolume 0x8030c2d0 (binary: `stfsu` straight into
   getSeParameter()+0x164 = vol slot 4) — no call, no tee. An interim guest-param-block
   mirror (mirror_guest_params: param block +0x124 vol / +0x1A4 pan / +0x224 pitch,
   8×16-byte {target,cur,step,count}, read cur each jai_tick, JAISound* bound from
   startSoundBasic r5) made distance attenuation work but reads guest engine state —
   an acknowledged stopgap, deleted by M2.6.
   **M2.6 — native 3D SE layer (NEXT, replaces the mirror — user-ratified direction:
   no guest-state mirroring; PC-native ownership before "fully working sound", because
   full correctness is unreachable while params come from the guest):**
   - Engine owns cameras, per-sound positions, and the SMS curves; tees capture only
     GAME INPUTS (positions/cameras/API calls), never guest audio state.
   - Camera: tee JAIBasic::setCameraInfo 0x80300ce4 (pos Vec*, dir Vec*, mtx, cam id).
   - Per-sound position: JAIActor (Vec* pos…) from the startSoundActor 0x80301e80 tee,
     bound to the ActiveSE; reading the game-world Vec each tick is engine INPUT, not
     mirroring.
   - Curves: port MSHandle::calcVolume / setDistanceVolumeCommon 0x8001c9ac (SMS
     overrides the generic JAISound curve!) incl. the static smSeCategory per-category
     max-distance table (read once from the DOL image) and JAIGlobalParameter constants.
   - Outputs feed the existing M2 move slots (vol/pan/pitch slot 4, doppler pitch slot 1).
   - KEY ENABLER (2026-06-12): overrides DO intercept recomp→recomp calls — every
     emitted `bl`/`bctrl` goes through call_ppc → recomp_lookup → override table. The
     old "overrides are blind to direct calls" gotcha was pre-C-call-model and is gone;
     the whole JAI surface can be natively owned with plain overrides.
4. **M4 — delete the guest path**: audioproc thread never started; JAS DSP/ucode/driver/mails
   removed from the audio chain; Dolphin DSPHLE unused.

## Ground rules
- Reference implementation is the decomp (decomp/sms/src/JSystem/JAudio) — port semantics,
  not emulate hardware. The oracle (DISABLE_RECOMP, with the pass-through guard) is the
  behavioral test: same scene, compare WAV pitch/envelope profiles.
- Data parsing in C++ mirrors tools/jingle/jingle.py exactly (it is the verified decoder).
- The native sink (native_audio.cpp) stays the device clock; the engine renders on demand
  from the sink's pull (true PC audio engine threading), not from emulated time.
- The recomp JAS stack stays in the binary but unreferenced once M4 lands; the
  seq-audio-dead wedge (CLAUDE.md frontier) becomes moot rather than fixed — note it as such.
