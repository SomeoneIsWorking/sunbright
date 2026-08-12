# Pick port targets from what the game ACTUALLY CALLS — and check your mangled-name lengths

2026-08-12, while looking for the next thing to port after TGuide.

## The worklist that matters is the one the runtime prints

`tools/re/gap_worklist.py` ranks unported decomp files by BYTE SIZE, which answers "what is the
most code" and not "what does the game reach". Sorting a Delfino run's `[STUB-CALLED]` lines
instead gives the set that is actually executed:

    TTalk2D2::perform            TTalk2D2::loadAfter
    TResetFruit::control         TAnimalBird::load  (marked "stopgap")
    TMtxSwingRZCallBack          TMtxTimeLagCallBack
    TGuide::perform              TModelWaterManager::drawShineShadowVolume
    GXPeekARGB

Nine stubs, each one a behaviour the game asks for and does not get. That is a better queue than
any static ranking, and it costs nothing — the loud-stub rule already produces it on every run.
The `[STUB-CALLED]` line prints once per stub, so the list is a SET, not a frequency.

## The mangling trap: I wrongly concluded five symbols were missing

Checking whether these had US addresses, I grepped `reference/sms_gmse01_funcs.txt` for
`control__12TResetFruit` and reported "not in US table" for four of five. **`TResetFruit` is
eleven characters, not twelve.** MWCC mangling encodes the class-name LENGTH, so an off-by-one in
that count matches nothing and looks exactly like an absent symbol.

Redone with the length computed rather than typed:

| class | mangled prefix | US symbols |
|---|---|---|
| `TResetFruit` | `__11TResetFruit` | 22 |
| `TAnimalBird` | `__11TAnimalBird` | 12 |
| `TTalk2D2` | `__8TTalk2D2` | 2 |
| `TGuide` | `__6TGuide` | 12 |

`TAnimalBird::load` — a reached stub, and one whose own message admits it is a stopgap — is at
**0x8000dea8**, 0x154 bytes. It was never missing.

**Never type the length.** `grep -E "__${#c}${c}F"` with the shell computing it, or search for the
bare class name and read what comes back. A "not in the US table" conclusion should be suspicious
by default: the table's real gap is WEAK methods (`vtable_re.py` exists for those), and a
non-weak, non-inline method of a class the game constructs is almost always present.

## TAnimalBird::load, resolved and ready

    TSpineEnemy::load(stream)
    stream.read(&eventID, 4)
    eventID >= 0 ? newAndRegisterObjByEventID(eventID, "鳥用")
                 : newAndRegisterObjByEventID(100, "")
    then switch on unk150->unk4c against 0x2000000f / 0x20000010 / 0x20000013

Both name operands are SDA2-relative and were read out of the image: `r2-0x7e40` is `"鳥用"`
(Shift-JIS, "bird-use") and `r2-0x7e38` is genuinely an EMPTY string — the byte at 0x8040ed68 is
zero, with the next non-zero data at +12 being the float 50.0. The first decoding to meaningful
Japanese is what validates the SDA2 base (0x80416ba0) used for both.
