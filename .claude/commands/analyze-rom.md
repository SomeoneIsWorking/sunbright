# Analyze ROM structure

Inspect the Super Mario Sunshine ROM without full recompilation.

## Steps

1. Ensure `sunbright-recomp` is built (see `/recompile`)
2. Run analysis mode:
   ```bash
   ./build/tools/recompiler/sunbright-recomp \
     "$SUNBRIGHT_ROM" \
     --analyze-only
   ```
   This prints:
   - Disc header (game ID, title, maker code)
   - DOL segment layout (text/data sections, entry point)
   - Function list (address, estimated size, # instructions)
   - Filesystem table (FST) summary
   - Opcode frequency histogram

3. For a quick DOL dump only:
   ```bash
   ./build/tools/recompiler/sunbright-recomp \
     "$SUNBRIGHT_ROM" \
     --dump-dol --output /tmp/sms.dol
   xxd /tmp/sms.dol | head -40
   ```

## What to look for
- Entry point address (typically 0x80003100 for SMS)
- BSS section bounds (zeroed at startup)
- Any overlays / dynamically loaded code (would need special handling)
- Confirm no REL modules are embedded in main DOL (SMS uses REL for objects)

## REL modules
SMS uses REL (relocatable modules) for most game objects. After analyzing the DOL,
check the disc filesystem for `*.rel` files — these need to be recompiled too.
Update this skill and CLAUDE.md when REL support is added.
