# Port a decomp function to native (`/patch-func <name|0xADDR>`)

For the ONE-RUNTIME native build (reference/sms compiled native): replace a
boot_stub OSPanic / fill an unported function with a faithful C++ port.

## 1. Generate the dossier (one command — does the extraction labor)
```
python3 tools/re/port_dossier.py <name-substr|0xADDR>
```
Prints a path to `scratch/re/dossier_*.md` with: auto-bounded disasm, whether the
decomp already has a C++ body, the boot_stub site, and porter notes.
(Needs `scratch/bin/sms.dol` — regenerate via `tools/re/dol_extract.c` if missing.)

## 2. Port
- **Decomp body EXISTS** (dossier §2 shows a `src/...` hit): the function is already
  written — the "port" is usually just removing the boot_stub and fixing a BE-swap /
  LP64 / uninit issue so the real body links & runs. Mechanical; delegate-friendly.
- **No decomp body** (cold RE): transcribe the disasm (§1) to C++. Cross-check
  lui/addiu sign-extension against Ghidra's decompiler; watch the LP64/BE gotchas in
  §4. If it calls an unported callee, run the tool on that address too. This is
  main-session brain-work (Sonnet ceiling), not agent labor.
- Delete the boot_stub in the SAME change; add a spec unit test from the RE
  (expected values hand-derived from the disasm) per the TDD-per-defect rule.

## 3. Build + verify (delegate this labor to a Sonnet agent)
```
cmake --build build --target sms-boot -j$(nproc)
SB_HEADLESS=1 SB_TURBO=1 SB_STAGE=<n> timeout -s KILL 30 ./run.sh 2>&1 | grep -aiE 'PANIC|STUB-CALLED <fn>'
```
Verify the OSPanic is gone and boot advances; commit test+port together.
