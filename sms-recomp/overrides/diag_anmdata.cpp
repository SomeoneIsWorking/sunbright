// diag_anmdata.cpp — name the animation directory an actor asks for, and say when it is not there.
//
// WHY THIS EXISTS. SBR_STAGE=9 dies on a null dereference inside MActorAnmData::init: the guest does
//
//     JKRFileFinder* f = JKRFileLoader::findFirstFile(dir);
//     do { addFileNum(f->mFileName); } while (f->findNextFile());
//
// with no null check (the decomp carries a SMS_NATIVE_PLATFORM guard for exactly this, but the
// recomp runs the game's real code, where the guard does not exist and cannot). findFirstFile
// returning null means the directory is not in any mounted archive, so the crash is three layers
// away from its cause: an asset that was never mounted, reported as a null pointer in an animation
// loader.
//
// The backtrace names the actor chain (load -> initMapObj -> makeMActors -> TMActorKeeper) but never
// the STRING, which is the one thing that identifies which archive is missing. This logs it.
//
// It is a pure observer: it reads the guest's argument and always runs the real body, so the crash
// still happens exactly where it did. `SB_LOG=anmdata` turns it on; silent otherwise.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <string>

extern "C" void func_8023c328(CPUState&);   // MActorAnmData::init(const char*, const char**)
extern "C" void func_8034b5ac(CPUState&);   // DVDConvertPathToEntrynum(const char*)
extern "C" void func_802c31b0(CPUState&);   // JKRFileLoader::findFirstFile(const char*)

namespace {

// Read a guest C string into a bounded buffer. Refuses rather than guesses: a pointer outside guest
// RAM prints as such, because "" for an unmapped pointer and "" for an empty string are different
// facts and this diagnostic exists precisely to tell them apart.
std::string guest_str(u32 ea) {
    if (ea == 0) return "<null>";
    if (sb_ram_fast(ea) == nullptr) return "<unmapped>";
    std::string s;
    for (int i = 0; i < 255; ++i) {
        const u8 c = sb_r8(ea + i);
        if (c == 0) break;
        s.push_back((char)c);
    }
    return s;
}

void ov_anmdata_init(CPUState& cpu) {
    lucent::debug("anmdata", "MActorAnmData::init(dir=\"{}\", extra=0x{:08x})",
                  guest_str((u32)cpu.gpr[4]), (u32)cpu.gpr[5]);
    func_8023c328(cpu);
}

// Same channel, because the two questions are one question: when an animation directory is missing,
// the next thing anyone asks is which disc file the stage actually mounted. The entry number is
// logged with it — -1 is the disc saying the path does not exist, which is a different failure from
// a file that opened and decoded wrong.
void ov_path_to_entrynum(CPUState& cpu) {
    const std::string path = guest_str((u32)cpu.gpr[3]);
    func_8034b5ac(cpu);
    lucent::debug("anmdata", "DVDConvertPathToEntrynum(\"{}\") -> {}", path, (s32)cpu.gpr[3]);
}

// THE ONE THAT MATTERS. MActorAnmData::init crashes when this returns null, and it returns null for
// a directory no mounted archive contains. Logging the path with its result turns "null pointer in
// an animation loader" into "this directory is not there" — and logging the SUCCESSES too is what
// makes the failure readable, because a lone failure line cannot say whether the archive is missing
// entirely or missing one directory.
void ov_find_first_file(CPUState& cpu) {
    const std::string path = guest_str((u32)cpu.gpr[3]);
    func_802c31b0(cpu);
    const u32 res = (u32)cpu.gpr[3];
    lucent::debug("anmdata", "findFirstFile(\"{}\") -> {}", path,
                  res == 0 ? std::string("NULL  <-- no mounted archive has this directory")
                           : std::string("ok"));
}

} // namespace

SB_OVERRIDE(0x802c31b0u, ov_find_first_file, "JKRFileLoader::findFirstFile",
            "diagnostic (SB_LOG=anmdata): report every directory lookup and whether it resolved — a "
            "null here is what MActorAnmData::init dereferences")
SB_OVERRIDE(0x8034b5acu, ov_path_to_entrynum, "DVDConvertPathToEntrynum",
            "diagnostic (SB_LOG=anmdata): name every disc path the game resolves, and whether the "
            "disc had it")
SB_OVERRIDE(0x8023c328u, ov_anmdata_init, "MActorAnmData::init",
            "diagnostic (SB_LOG=anmdata): log the animation directory an actor asks for — a "
            "directory absent from the mounted archives crashes inside this function with no clue "
            "which one it was")
