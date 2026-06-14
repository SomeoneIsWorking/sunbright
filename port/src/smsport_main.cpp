// =============================================================================
// Super Mario Sunshine — native PC port: minimal boot smoke entry point.
//
// This is a SCOPING probe, not the real game entry. Its job is to force the
// linker to resolve everything libsmsport_core.a (+ assets + texdecode) needs,
// so the set of UNDEFINED platform-layer (GameCube SDK) symbols becomes
// visible. That undefined-symbol surface is the PAL (platform abstraction
// layer) implementation worklist for a minimal native boot.
//
// It deliberately does almost nothing. A trivial main() does NOT pull any core
// objects in (a static archive only contributes objects that satisfy an
// undefined reference), so we cannot rely on the link alone to surface the core
// lib's needs. The real inventory is produced by `nm`-diffing defined vs
// referenced symbols across the archives (see the report task). This target
// exists so there is a buildable consumer to link against and to host any
// future native boot bring-up.
// =============================================================================
int main(int /*argc*/, char** /*argv*/)
{
    return 0;
}
