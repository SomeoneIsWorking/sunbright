# Update project documentation

Run this after any significant code change to keep docs and CLAUDE.md in sync.

## Steps

1. **Check instruction coverage** — scan ppc_decoder.cpp for handled opcodes and update the table in CLAUDE.md
   - Search for `case OP_` or `case 0x` patterns in ppc_decoder.cpp
   - Cross-reference against the table in CLAUDE.md

2. **Check for new unhandled opcode warnings** — grep the recompiler output or source:
   ```bash
   grep -n "UNHANDLED\|TODO\|FIXME\|unimplemented" tools/recompiler/*.cpp tools/recompiler/*.h
   ```

3. **Update docs/architecture.md** if:
   - New pipeline stages were added
   - Dolphin integration approach changed
   - New file categories were created

4. **Update docs/port/native_plan.md** if:
   - New instruction categories were added
   - Emitter output format changed
   - New intrinsics were added to runtime/intrinsics.h

5. **Update (retired: Dolphin substrate, see CLAUDE.md)** if:
   - Hook mechanism changed
   - New Dolphin subsystems are being used
   - Memory mapping approach changed

6. **Update CLAUDE.md instruction table** (copy from docs/port/native_plan.md#coverage)

7. **Commit docs alongside code changes** — docs that trail behind code are worse than no docs.

## Self-evolving reminder
If you (Claude) find yourself repeatedly explaining something that isn't in the docs,
add it to the docs and update this skill to remind future-Claude to check there first.
