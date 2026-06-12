# Native audio RE findings — pitch/gain defects + fxmix/dolby port plan (2026-06-12)

Research-only session: reverse-engineering for the open native-audio defects listed in
CLAUDE.md ("OPEN (quantified, post-fix)"). Ground-truth chain used throughout:

1. **Binary disassembly** (`sunbright-recomp --disasm`, addresses from
   `reference/sms_gmse01_funcs.txt`) — final authority.
2. **reference/sms decomp** — used as a map; its `TInstParam` field NAMES were verified
   against the binary frame offsets below (they are correct; an earlier session's claim
   that they were crossed is itself wrong — see §2.3).
3. **Parsed ROM data** (`SUNBRIGHT_DUMP_IBNK` → `scratch/logs/ibnk_dump.txt`, plus a
   direct mSound.aaf parse) — to test whether a defect is data-driven.
4. **Existing A/B captures** (`scratch/logs/delfino_{oracle,native}.jsonl`) — to
   quantify each hypothesis without running the game (this session ran nothing).

Defect entries quantified from the captures (hash = wave srcHash join key):

| defect | bank:prog (key) | hash | oracle ratio med | native ratio med | Δ | oracle vol med |
|---|---|---|---|---|---|---|
| 1 | 10:1 k24 | c4261b30 | 3991 | 1410 | **−1801¢** | 32766 |
| 2 | 11:0 k75-82 | fbba4053 | 5787 | 4867 | **−300¢** | 19225 |
| 2 | 5:229 k10 | 30f740b7 | 2774 | 2046 | **−527¢** | 18060 |
| 2 | 5:229 (other keys) | 14e9dc33 | 4591 | 3954 | **−259¢** | 26575 |
| 3 | 6:77 k60 | 51eb0b8d | 2293 | 1996 | −240¢ | **0** |
| 3 | 6:231 k7 | 8b9c612a | 2717 | 2712 | −3¢ | **103** |

---

## 1. Defects 1 & 2 (and the pitch component of 3): track pitch-bend scale is 4× too small

**ROOT CAUSE (binary-verified): `native_jas.cpp pitch_to_ratio()` divides the bend
exponent by 48; the real `pitchToCent` divides by 12.**

### Original code path

- `JASystem::Player::pitchToCent(f32 pitch, f32 range)` @ **0x80319e30**
  (decomp `JASPlayer_impl.cpp:119`, structure confirmed instruction-by-instruction):

  ```
  v      = pitch * 4.0f * range          // consts at rtoc 0x760/0x764
  whole  = (s16)v;  frac = v - whole     // fctiwz + borrow fixup for v<0 / frac>=1
  return s_key_table[(u16)(frac*64)]     // table @ 0x803e3488, 64 entries
       * C5BASE_PITCHTABLE[whole + 60]   // table @ 0x803e2880 (lfs 0xF0(r3) = idx+60)
  ```

  `s_key_table[i] = 2^(i/768)` (last entry 1.0589809 = 2^(63/768)·…, i.e. the table
  spans exactly ONE SEMITONE over 64 steps) and `C5BASE[i] = 2^((i-60)/12)`. Therefore

  **`pitchToCent(pitch, range) = 2^(pitch * 4 * range / 12)`** — `pitch*4*range` is in
  **semitones**.

- Caller: `TTrack::updateTrackAll` @ 0x8031af2c / `updateTrack` @ 0x8031b2e4
  (`JASTrack.cpp:384`): `curPitch = pitchToCent(mTimedParam.mPitch.cur, mRegisterParam.unkE)`;
  `unkE` (pitch range) init = **0x0C** (`TRegisterParam::init`, `JASRegisterParam.cpp:35`
  — matches native `pitchRange = 0x0c`).
- `mPitch.cur` is the BMS timed-param value / 32768 (matches native `write_time_param`).

So with the default range 12, full-scale bend (0x7FFF) = **48 semitones**; a BMS value
of 0x4000 = 24 semitones (2 octaves).

### What native does differently

`runtime/native_jas.cpp`:

```c
static float pitch_to_ratio(float pitch, float range) {
    float v = pitch * 4.f * range;
    return exp2f(v / 48.f);     // WRONG: should be v / 12.f
}
```

Every BMS pitch bend lands at **1/4 of its intended size**. The error per voice is
`0.75 × intended bend`:

- 10:1: oracle/native = 2^(18/12) → intended bend **24 semitones** (BMS value 0x4000,
  a clean two-octave offset) — native applies 6, hence "~16-18 semitones low".
- 11:0 −300¢ → intended bend 4 semitones (value 0x0AAA), native applies 1.
- 5:229 drum keys ±300¢-class errors → same mechanism on the drum tracks' bends
  (drums take chPitch like any voice; their **instrument** data is all-neutral, §2.1).
- 6:77 −240¢ pitch component → same.

### Porting recommendation

In `pitch_to_ratio`, change `exp2f(v / 48.f)` → `exp2f(v / 12.f)` (optionally replicate
the exact table quantization: `2^(whole/12) * 2^(floor(frac*64)/768)` with the binary's
truncation-toward-zero + negative-frac fixup; the analytic form is within 0.02¢ of the
tables, far below audibility — the quantized form only matters if we ever want
bit-exact A/B ratios).

**Predicted result:** defects 1 and 2 collapse to ~0¢; 6:77's −240¢ also clears.
Verify with `tools/audio/delfino_ab.sh` — residual per-wave cents should be |Δ| < 10.

Note `pitch_to_cent()` directly above `pitch_to_ratio` in native_jas.cpp is dead code
(never called, returns a half-finished expression) — delete it when fixing.

---

## 2. Drum (perc) data semantics — binary truth + one real native bug

### 2.1 The defect keys' data is neutral — pitch errors there are the §1 bend bug

`bank5 prog229` keys 10/78/79, `bank6 prog231` key 7, `bank10 prog1`, `bank11 prog0`,
`bank6 prog77`: ALL have vol=1.0 pitch=1.0 volmul=1.0 pitchmul=1.0 in the parsed dump.
No data-driven pitch/gain difference is possible for these entries. (Dead end: per-key
instrument data as the cause of defect 2/3.)

### 2.2 Binary-verified TInstParam/noteOn layout (settles the decomp-vs-decomp confusion)

`BankMgr::noteOn` @ **0x8030dc7c**: the local `TInstParam` lives at **sp+0x24**
(`addi r6,r1,0x24` before the virtual `getParam` call). Decoding the field uses:

| TInstParam offset | meaning | use in noteOn |
|---|---|---|
| +0x00 | u8 (osc-key flag, 0) | chanKey switch |
| +0x08/+0x0C | oscData/oscCount | setOscInit loop |
| +0x10 | **volume** | `chan->unk4C` (vol base) — `lfs 0x34(r1)` |
| +0x14 | **pitch** | `chan->unk48 = [+0x14] * rate/dac` — `lfs 0x38(r1)` |
| +0x18 | vol-effect product (target 0) | `unk54 *=` — `lfs 0x3C(r1)` |
| +0x1C | pitch-effect product (target 1) | `unk50 *=` — `lfs 0x40(r1)` |
| +0x20/+0x24/+0x28 | pan/fx/dolby **Sound** | → chan unk68/unk74/unk80 `.mSound` |
| +0x2C/+0x30/+0x34 | pan/fx/dolby **Effect** (targets 2/3/4) | → `.mEffect` |
| +0x38 | fixed-pitch flag | skips the C5BASE key-scaling |
| +0x3A | direct release | `directReleaseOsc(0, …)` |

This MATCHES the reference/sms decomp's field names (`unk10`=vol, `unk14`=pitch,
target0→`unk18` vol, target1→`unk1C` pitch). The native source comment claiming the
decomp crossed these ("target 0 is VOLUME, target 1 is PITCH … decomp reads unk18 in
both spots") reached the right effect-target mapping but for the wrong reason: the
decomp's *named* mapping is correct; only `JASBankMgr.cpp`'s noteOn body has unk14/unk18
typos. Native behavior for melodic insts and effects is **correct** as-is.

### 2.3 ⚠ REAL BUG: native reads the file TPmap vol/pitch SWAPPED

Binary chain for percussion:

- `BNKParser::createBasicBank` @ 0x8030f26c, pmap copy @ **0x8030f794**:
  `perc+0 = pmapRaw+0; perc+4 = pmapRaw+4` — a **straight copy** (lfs 0(r28)/stfs 0(r3),
  lfs 4(r28)/stfs 4(r3)).
- `TDrumSet::getParam` @ **0x8030fc4c**: `instParam+0x10 *= perc+0` (VOLUME),
  `instParam+0x14 *= perc+4` (PITCH); also `+0x20 = perc+8` (pan), `+0x3A = perc+0xC`
  (release), `+0x38 = 1` (fixed pitch).

**Therefore the FILE layout is `pmap+0 = volume, pmap+4 = pitch`.** The data agrees:
every non-unity `pmap+4` value in SMS is semitone-quantized (0.7071=2^-6/12,
0.8409=2^-3/12, 1.6818=2^9/12, 1.1225/1.1892/1.2599/1.0595 = 2^{2,3,4,1}/12) — these are
pitches. `native_jas.cpp parse_ibnk` currently does the opposite:

```c
pc.pitch = bef32(pmap);        // actually VOLUME
pc.vol   = bef32(pmap + 4);    // actually PITCH
```

This replicated pikmin2's crossed `TPmap` field naming — exactly the documented trap.
The CLAUDE.md note "perc TPmap pitch@0 (oracle-verified)" is **falsified**: the 0:233
−30 dB/+105¢ signature that "verified" it was actually fixed by the simultaneous PER2
pan change (pan/127 was previously multiplied into volume); 0:233's vol/pitch are both
1.0, so the swap was a no-op for that program and rode along unverified.

Blast radius: small but real — only ~8 non-unity TPmap entries in SMS (bank0
prog228 k41/k43/k65, prog229 k91, prog230 k57/k60/k62, bank12 prog230 k82). Today
native plays e.g. 0:228 k41 at −3 dB/correct-pitch instead of 0 dB/−6 semitones, and
0:229 k91 at +4.5 dB/correct-pitch instead of 0 dB/+9 semitones.

**Recommendation:** swap the two reads back (`pc.vol = bef32(pmap); pc.pitch =
bef32(pmap+4);`), update the misleading comments here and in CLAUDE.md, and re-run the
A/B on a scene that hits bank0 prog228-230 (Delfino plaza drums/percussion are 5:229 —
all-neutral, so the swap cannot regress the current report).

---

## 3. Defect 3 (6:77 / 6:231k7 "+13..25 dB loud natively"): not a gain bug — missing fx/dolby routing

Data: both programs have fully neutral vol fields (§2.1), and the native volume chain
(`chVol × vol × oscVol`, vel²-law, volumeMode squaring) matches `updateTrackAll` /
`updateEffectorParam` / noteOn semantics line for line. There is no native gain math
error to find.

The oracle capture explains it instead: these voices' **oracle-side measured volume is
~zero** (51eb0b8d vol median **0**, 8b9c612a **103**, vs 18060–32766 for normal SEs).
The harness reads `dolby_volume_current` for dolby voices and max-channel volume
otherwise — i.e. on the real engine these voices are routed almost entirely OFF the
measured dry/dolby path (high fxmix and/or dolby with sin-law attenuation of the front
buses), while the native engine plays them dry at full volume. The "+13..25 dB" is the
relative-volume normalization of the report seeing native-dry vs oracle-attenuated.

Mechanism in the original (see §5 for the full chain): in `Driver::updateMixer`
(JASChannel.cpp:294, mix-config path) each of the 6 bus volumes is
`vol × sinfT(sel0) × sinfT(sel1)` where the selectors can be `pan/fxmix/dolby` or their
complements — a front bus configured with `1-fxmix` or `1-dolby` loses 10–30 dB when
the mix is pushed toward the effect bus. In the auto-mixer path the Zelda ucode applies
the equivalent law from the `setAutoMixer(vol*32767.5, pan*127.5, dolby*127.5,
fxmix*127.5)` parameters.

**Recommendation:** do NOT add a per-program gain workaround (that would be a bandaid on
top of a missing subsystem). Implement defect 5 (fxmix/dolby buses, §5) — the dry-path
attenuation comes with it for free — then re-measure. If 6:77 is still hot afterwards,
re-investigate with the per-voice fxmix/dolby values visible in `/njas`.

---

## 4. Defect 4 (~10 oracle-only waves per run): candidate native drop paths

No single smoking gun; these are the enumerated ways the native engine refuses/loses a
note that the guest plays, in estimated likelihood order:

1. **Native-only request heuristics** in `process_pending()` / `jai_tick()`
   (native_jas.cpp): per-id concurrency cap (`idCount >= 4` steals oldest), lifeTime-10
   expiry for continuous-class ids, "never busy in 60 ticks" release, 800-subframe
   pending expiry. The guest's actual rules live in JAIBasic's category buffers
   (`JAIBasic::checkEntriedSeRegist` / per-category `seRegist` counts) and differ per
   category — our constants are approximations.
2. **Voice stealing scope**: native steals only RELEASING voices; the real
   `TChannelMgr::getLogicalChannel` / DSP `breakLower` steal by priority across playing
   voices. A 64-voice-full moment rejects new notes natively (`OUT OF VOICES`).
3. **Wave-group residency**: noteOn returns −1 when the id is missing from the selected
   scene group (logged `wave %u missing`). Group fallback hides most of this, but a
   guest `loadGroupWave` tee that arrives late loses notes started in between.
4. **`noteOnOsc`** (prog ≥ 0xF0, BankMgr @ 0x8030e08c): oscillator-table synth voices —
   native rejects (no inst). These have no ARAM wave on the oracle either, so they are
   NOT in the missing-wave list, but they are silently dropped natively.
5. Untee'd start paths: anything that doesn't funnel through `startSoundBasic`
   0x803020ac (believed none for SE/BGM; streams are out of scope).

**Recommendation (instrumentation first, then fix):** add a reject-reason counter per
noteOn/-request drop (`no-worker`, `expired`, `cap-steal`, `lifeTime`, `no-voice`,
`wave-missing`, `no-inst`) dumped via `/njas`, run the standard A/B scene, and join the
oracle-only hash list against the reject log. Fix whichever bucket dominates — most
likely replacing the cap/lifeTime constants with the real JAIBasic per-category logic
(`reference/sms/src/JSystem/JAudio/JAInterface`), which is also the M4 direction.

---

## 5. Defect 5: fxmix/dolby — original semantics + native port plan

### 5.1 How JASystem computes fxmix/dolby per voice (all decomp-verified, names sane)

Three layers, exactly parallel to volume/pan:

1. **Track** (`TTrack::updateTrackAll` @ 0x8031af2c):
   - `curFxmix = mTimedParam.mFxmix.cur` (BMS timed param **2**), `curDolby = …mDolby.cur`
     (timed param **4**) — native already tracks both (`timed[2]/timed[4]` → `chFx/chDolby`).
   - Outer params: switch bit **4** = fxmix, bit **0x10** = dolby, combined via
     `panCalc(cur, outer, panPower[3]/32767, mode)` (modes: 0=keep, 1=replace, 2=lerp).
   - Parent combine (non-root, !(trackMode&1)):
     `chFx = panCalc(curFxmix, parent.chFx, panPower[4]/32767, unk3C8[1])`, same for
     dolby — **native currently skips the parent combine and outer for fx/dolby**
     (`chFx = fx` only).
2. **Channel** (`TChannel::updateEffectorParam`, JASChannel.cpp:852):
   - `fxmix = calcEffect(&unk74, &power, calcMode)` where `unk74 = {mSound (from
     instParam +0x24, default 0), mEffect (instParam +0x30, set by inst SENSE/RAND
     effects target 3), mChannel (= track chFx)}`.
   - `calcEffect` sums the three components per `calc_sw_table[mode]`
     (JASChannel.cpp:29); the TChannelMgr default mode is **13 = ADD all three**
     (`unk62[i]=0xD`, JASChannelMgr.cpp:52). Same for dolby (components from instParam
     +0x28/+0x34, target 4). Pan uses `calcPan` = same but components are `(x-0.5)`
     offsets re-centered on 0.5 — note native pan currently drops the `.mEffect` term
     (instParam +0x2C, effect target 2); add it while in here.
   - dolby is only computed at all when `Driver::getOutputMode() == 2`; the SMS oracle
     demonstrably produces dolby-routed voices (`use_dolby_volume` VPBs), so SMS runs
     output mode 2.
3. **Mixer** (`Driver::updateMixer` / `updateAutoMixer`, JASChannel.cpp:287..):
   - Auto-mixer path (`unkA8[0] == 0xFFFF`, the JAS default):
     `setAutoMixer(vol*32767.5, pan*127.5, dolby*127.5, fxmix*127.5)` — the DSP ucode
     derives: dry L/R from vol×pan law, **fx send** (reverb input) from fxmix, rear
     extraction from dolby. Dolphin's `ZeldaAudioRenderer` is the line-by-line reference
     for the exact ucode law (we already vendored its tables for the native DAC ucode).
   - Manual path: per-bus `MixConfig` nibbles select scale = pan/fxmix/dolby or
     complement; `busVol = vol × sinfT(scale0) × sinfT(scale1) × mixerLevel`,
     `sinfT(x) = sin-table[x*256]` ≈ `sin(x·π/2)` (a quarter-wave table; dolby uses a
     separate `sinfDolby2` table for its bus).

   The **effect buses** feed the ucode reverb/chorus units whose parameters
   (delay/coef tables) JAI uploads per stage; on hardware that's the AUX bus processing.

### 5.2 How Dusklight ported this natively (scratch/ref/dusklight/src/dusk/audio/DuskDsp.cpp)

- One **shared stereo freeverb** (`libs/freeverb/revmodel`, wet=1 dry=0 roomsize=0.5
  damp=0.7 width=1) for the whole mixer.
- Per rendered voice subframe: `inputGain = (mAutoMixerFxMix >> 8) / 600.0f`; the
  voice's post-pan L/R subframe × inputGain accumulates into a reverb input bus. Key
  design note from their comments: **scale the reverb INPUT, not wet/dry on the
  output** — the tail then decays correctly when fxmix changes mid-note (no transients).
  (600 is their tuned constant, "sounds good enough vs console".)
- `revmodel::processmix(inL, inR, outL, outR, n, 1, 1.0)` adds wet into the main mix;
  a `ReverbHasTail` energy gate (−80 dBFS epsilon) skips processing when silent.
- Dolby: treated as a surround EXTRACTION — `extract = dolby × 0.6`, pull
  `mono × extract` into a surround bus (LPF'd + allpass-decorrelated back into L/R when
  HRTF enabled), front scaled by `1 − extract`. With it disabled they simply ignore
  dolby (front stays full).

### 5.3 Port plan for native_jas.cpp (faithful-first)

1. **Track plumbing** (cheap, do first): apply outer fxmix/dolby switches (4/0x10) and
   the parent `panCalc` combine for `chFx`/`chDolby` in `track_update`; add the
   per-voice `fxSound/fxEffect/dolbySound/dolbyEffect` from instParam (+0x24/+0x30,
   +0x28/+0x34 — parse_inst currently ignores these file fields; the INST file
   offsets feeding them come from the effect records with targets 3/4 and the
   inst param block) and compute per-voice `fx = clamp01(fxSound + fxEffect + chFx)`,
   `dolby = clamp01(…)` (calc mode 13 = add-all default). Tee
   `JAISound::setFxmix`-class setters if the game uses them (check inner setter
   neighbors of 0x8030b700 in the funcs map).
2. **Reverb bus** (Dusklight pattern): vendor freeverb (BSD/public-domain), one shared
   stereo instance at 32028.5 Hz inside `render_subframe`; per voice add
   `sample × gl/gr × fxgain` into `fxbufL/R` where `fxgain = sinfT(fx)`-law
   (use `sinf(fx·π/2)`); after the voice loop run
   `processmix(fxbuf → g_mixbuf)` with a tail-energy gate. Start with Dusklight's
   tuning (wet-only, input-scaled, ÷~600-equivalent normalization → calibrate by A/B
   RMS against the oracle on a reverb-heavy stage like Noki Bay / inside buildings).
3. **Dry-path attenuation**: when fx/dolby are live, scale the dry contribution the way
   the auto-mixer does (front = vol × (1−dolby·k) and the ucode's fx complement;
   take the exact law from Dolphin `ZeldaAudioRenderer`'s voice-mixing code rather than
   guessing — that is what the oracle measurement reflects). This is the piece that
   clears defect 3.
4. **Dolby**: fold into stereo à la Dusklight — front scale `1 − dolby·0.6` (skip the
   HRTF surround return initially; we output 2.0). Faithful enough until we ever do
   real surround.
5. Re-run `tools/audio/delfino_ab.sh`; expect 6:77/6:231 relative dB to collapse and
   overall volume medians to shift slightly (renormalized).

---

## 6. Dead ends / falsified (do not re-walk)

- **Wave-group (scene) collision as the cause of defect 1/2**: the defect wave ids
  (28/29, 14-16, 382, 5, 98, 158/159) exist ONLY in group 0 of wsys 1 — no collision
  possible; metadata (key/rate) is unambiguous. (Collisions remain a theoretical risk
  for wScene/wsys 2 ids.)
- **Instrument/keymap data as the cause of defects 1–3**: every involved INST/PER
  entry is fully neutral (vol=pitch=volmul=pitchmul=1.0). Verified from the file, not
  the runtime.
- **Native melodic pitch math** (`baseRatio = ipitch × rate/dac × pitchEff`, C5BASE
  `key+60−wavekey` clamp 0..127, fixed-pitch for percs): matches noteOn @0x8030dc7c
  exactly. The only pitch defect is the §1 bend scale (plus §2.3 for 8 perc entries).
- **"decomp TInstParam fields are crossed"** (old native comment): wrong — binary frame
  decode shows the decomp names are right; only noteOn's *body* in the SMS decomp has
  unk14/unk18 transcription typos (it reads `unk14` for pitch-base — true layout
  +0x14 IS pitch — and `unk18` where the binary uses +0x1C for the pitch-effect).
- **"perc TPmap pitch@0, oracle-verified"** (CLAUDE.md M2.6 note): falsified, §2.3 —
  the observed fix came from the PER2 pan-as-volume correction shipped in the same
  change; binary + semitone-quantized data prove vol@0 / pitch@4.
- `pitch_to_cent()` in native_jas.cpp is dead, half-written code — never called.
- **"SE worker tree runs ~12x slower natively" (2026-06-12): FALSIFIED — measurement
  artifact.** Two compounding clock bugs in the A/B instrument, no engine defect:
  1. A past `SUNBRIGHT_TURBO=1` run PERSISTED `EmulationSpeed = 0` into
     `<home>/.config/dolphin-emu/Dolphin.ini` (same class as the DumpFrames gotcha), so every
     "real-time" oracle ran unthrottled at a *fluctuating* 2–10x. Fixed: `main_sdl.cpp`
     now sets `MAIN_EMULATION_SPEED` explicitly every run (1.0, or 0 under TURBO).
  2. A/B event timestamps were WALL-clock on both sides; oracle turbo bursts compressed
     onset spacing arbitrarily. Fixed: oracle events now stamp EMULATED time
     (CoreTiming ticks, vpb_trace.cpp), native events stamp the engine's subframe clock
     (g_ab_subframes x 2.4977 ms), and oracle voice durs are in subframe units (=native).
  Verified binary/live ground truth along the way (all matching the native port):
  `updateTempo` 0x8031b814 = timebase*tempo/dacRate*80/60; live oracle SE root+workers
  all tick at tickPerCall 0.2398 (96 ticks/s, tempo 120 timebase 48); rootCallback runs
  per SUBFRAME (`DSPBuf::updateDSP` → `Kernel::subframeCallback`); gates are in subframe
  units on both sides (`TChannel::play` unk30/unk34, decremented by updateInterval per
  param-0 update); envelope rate = envTime*(dacRate/80/600)/updateInterval — the
  interval cancels out of wall time, so native's interval==1 assumption is exact.
  Emu-clock A/B (ab_report4): the arpeggio LIFE/onset divergences are gone.
