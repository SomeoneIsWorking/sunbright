# Build the project

## First-time setup (run once)

```bash
git submodule update --init --recursive externals/dolphin
```
This initializes Dolphin's nested submodules (fmt, pugixml, zstd, etc.). ~500 MB download.

## Full build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Individual targets

```bash
cmake --build build --target sunbright-recomp    # Offline recompiler tool
cmake --build build --target sunbright-runtime   # Runtime (links Dolphin)
```

## After build, verify

```bash
./build/tools/recompiler/sunbright-recomp --version
file ./build/runtime/sunbright-runtime
```

## Troubleshooting common build failures
- Missing Dolphin headers: `git submodule update --init --recursive externals/dolphin`
- Missing Qt/wxWidgets: runtime doesn't need them, set `-DENABLE_QT=OFF -DENABLE_WXWIDGETS=OFF` if Dolphin demands them
- DiscIO won't build standalone: check `cmake/DolphinLibs.cmake` for correct target names
- Paired singles missing: check `tools/recompiler/ppc_decoder.cpp` primary opcode 4

## Self-update instruction
When build system changes (new files, new Dolphin libs needed, new compiler flags),
update this skill AND `CMakeLists.txt` comments so both stay in sync.
