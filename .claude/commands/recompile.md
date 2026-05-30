# Recompile ROM → native functions

Recompile Super Mario Sunshine from the ROM into native C code.

## Steps

1. Verify ROM exists at `$SUNBRIGHT_ROM`
2. Verify the recompiler tool is built: `ls build/tools/recompiler/sunbright-recomp`
   - If missing, run: `cmake -B build && cmake --build build --target sunbright-recomp -j$(nproc)`
3. Run the recompiler:
   ```bash
   ./build/tools/recompiler/sunbright-recomp \
     "$SUNBRIGHT_ROM" \
     --output generated/
   ```
4. Report:
   - Number of functions recompiled
   - Any unhandled opcodes (update CLAUDE.md instruction table if found)
   - Any functions flagged as indirect-jump-heavy (may need manual review)
5. If new unhandled opcodes were found, add them to `ppc_decoder.cpp` + `c_emitter.cpp` and rerun
6. After success: `cmake --build build --target sunbright-runtime -j$(nproc)`

## Updating after adding new instruction support
Re-run this skill any time ppc_decoder.cpp or c_emitter.cpp changes to get better coverage.
