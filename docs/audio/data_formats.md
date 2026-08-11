# SMS audio data — PC-native access (verified 2026-06-11)

The boot jingle (and all SE/BGM wave data) is fully decodable from the ROM with zero
emulation. Verified by ear: `w1stLoad_0.aw` wave 2 IS the boot-jingle sound.

## Where the data lives
- `/AudioRes/Banks/*.aw` — raw wave archives (no headers; offsets/rates live in the WSYS).
  `w1stLoad_0.aw` (64 KB) = the boot bank: 5 waves, loaded before anything else.
- `mSound.aaf` — JAudio init data (BST/BSTN/WSYS/IBNK tables). NOT a disc file: it lives
  **inside `/data/nintendo.szs`** (Yaz0-compressed RARC, file name lowercased to
  `msound.aaf`), loaded by `TApplication::initialize_bootAfter` and passed to `MSound`
  as a pointer (`decomp/sms/src/System/Application.cpp:303`).
- `/AudioRes/mSound.asn` — sound-ID name table (text), not init data.
- `/AudioRes/Seqs/sequence.arc` — BMS sequences; `/AudioRes/Streams/title.afc` — streamed DTK.

## Formats (all parsed by `tools/jingle/jingle.py` (DELETED))
- **Yaz0**: standard RLE back-reference scheme; header `Yaz0` + u32 decompressed size.
- **RARC**: nodes @+0x24, file entries @+0x2C, strings @+0x34, data @ +0xC (+0x20 each).
- **AAF**: chunk-ID stream. IDs 1/4/5/6/7 = single (offset,size,flag); IDs 2/3 = list of
  (offset,size,flag) until offset==0. ID 3 entries are WSYS blobs.
- **WSYS**: `WSYS` … u32@0x10 → `WINF`: u32 count + offsets to per-.aw entries.
  Per-.aw entry: char name[0x70], u32 wave_count, u32 wave_entry_offsets[].
  Wave entry: +1 u8 format, +2 u8 base key, +4 f32 sample rate, +8 u32 start (byte offset
  into the .aw), +0xC u32 length, +0x10 loop flag (+0x14/+0x18 loop points).
- **Wave format byte**: 0 = AFC 4-bit HQ (9-byte blocks), 1 = AFC 2-bit (5-byte blocks),
  2/3 = PCM8/PCM16.
- **AFC decode** (= what the DSP does, cf. Dolphin `ZeldaAudioRenderer::DecodeAFC`):
  block header byte = `(delta_exp << 4) | coef_index`; 16 nibbles follow;
  `sample = (delta*nib + yn1*c0 + yn2*c1) >> 11`, nibble sign-extended then `<<11`.
  Canonical 16-pair coefficient table in `tools/jingle/jingle.py` (DELETED) (verified correct by ear).

## Gotcha that produced "garbage audio"
Decoding an .aw as one flat AFC stream does NOT work: each wave starts at its own WSYS
offset, so the 9-byte block grid misaligns at every boundary and the predictor history is
wrong. Always split per the WSYS wave table first.

## Tools
- `build/sunbright-jingle <rom> [--list] [--extract <prefix> <outdir>]` — DiscIO-based FST
  lister/extractor (`tools/jingle/jingle_extract.cpp` (DELETED)).
- `tools/jingle/jingle.py <nintendo.szs> <banks_dir> <outdir>` — Yaz0→RARC→AAF→WSYS→AFC,
  writes one WAV per wave at its true sample rate.
- `tools/jingle/afc_decode.py` (DELETED) — raw AFC-stream decoder (alignment caveat above).
- `tools/audio/run_check.sh [secs] [ENV=V…]` — headless run + per-second WAV profile.

## Boot-jingle ground truth
`w1stLoad_0.aw` waves: w00 0.83 s @22050, w01 0.24 s @22050, **w02 1.03 s @32000 = the
jingle**, w03 1.16 s @32000, w04 0.66 s @32000. The logo code path is
`GCLogoDir` → `startSoundSystemSE(MSD_SE_MV_CHAO = 0x7914)` at fade-in.
