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
#include "../frame_interp/graphics_db.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void func_8023c328(CPUState&);   // MActorAnmData::init(const char*, const char**)
extern "C" void func_8034b5ac(CPUState&);   // DVDConvertPathToEntrynum(const char*)
extern "C" void func_802c31b0(CPUState&);   // JKRFileLoader::findFirstFile(const char*)
extern "C" void func_802bc9f8(CPUState&);   // JKRArchive::JKRArchive(s32 entrynum, EMountMode)
extern "C" void func_802bc9ac(CPUState&);   // JKRArchive::JKRArchive()  — the OTHER base ctor
extern "C" void func_802c3124(CPUState&);   // JKRFileLoader::findVolume(const char**)
extern "C" void func_802c3740(CPUState&);   // JKRHeap::alloc(u32 size, int align, JKRHeap*)
extern "C" void func_802bf41c(CPUState&);   // JKRArchive::getFirstFile(const char*) const
extern "C" void func_802bcb14(CPUState&);   // JKRArchive::findDirectory(const char*, u32) const
extern "C" void func_802c3c38(CPUState&);   // operator new(size_t, JKRHeap*, int)
extern "C" void func_802c1edc(CPUState&);   // JKRExpHeap::getFreeSize()
extern "C" void func_802c37b8(CPUState&);   // JKRHeap::free(void*, JKRHeap*)
extern "C" void func_802c1b14(CPUState&);   // JKRExpHeap::free(void*)
extern "C" void func_802c1f48(CPUState&);   // JKRExpHeap::getTotalFreeSize()
extern "C" void func_802c138c(CPUState&);   // JKRExpHeap::alloc(u32 size, int alignment)
extern "C" void func_802c1b88(CPUState&);   // JKRExpHeap::freeAll()
extern "C" void func_802c52b0(CPUState&);   // JKRThread::JKRThread(u32 stackSize, int, int)
extern "C" void func_802c14d0(CPUState&);   // JKRExpHeap::allocFromHead(u32, int)
extern "C" void func_802c18dc(CPUState&);   // JKRExpHeap::allocFromTail(u32, int)
extern "C" void func_802c17b0(CPUState&);   // JKRExpHeap::allocFromHead(u32)  — default alignment
extern "C" void func_802c1a34(CPUState&);   // JKRExpHeap::allocFromTail(u32)  — default alignment
extern "C" void func_802d4cf0(CPUState&);   // unnamed J3DSkinDeform-region function that recurses
extern void rt_dump_guest_stack(const char* why);

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

// ── WHAT IS ACTUALLY IN THE MOUNTED ARCHIVE ─────────────────────────────────────────────────────
//
// Issue #1: stage 9 dies because findFirstFile("/scene/mapObj") returns null, and the two candidate
// causes need opposite fixes — the directory is genuinely absent from mare0.szs, or our mount of it
// is incomplete. Nothing measured so far can separate them, and both readings are consistent with
// every number available (the DVD log shows the whole file streaming in cleanly; the RARC directory
// table sits at the FRONT so late corruption could not drop an entry; but a retail disc whose Noki
// Bay had no mapObj directory would crash on console too, which cannot be true).
//
// So this walks the archive's OWN node table and prints the directory names. Layout from the decomp
// (JKRArchive.hpp): mArcInfoBlock at +0x44 { num_nodes, node_offset, num_file_entries,
// file_entry_offset, string_table_length, string_table_offset }, mDirectories at +0x48, mStrTable
// at +0x50, and each SDIDirEntry is { type, nameOffset, hash, num, firstIdx } — the name being an
// offset into the string table. The volume name lives in JKRFileLoader at +0x28.
//
// Archives are collected by hooking the JKRArchive constructor rather than by chasing the loader's
// static list, because the list is a JSU linked list whose layout would be one more thing to get
// right, and a constructor hook cannot miss a mount that happened.
constexpr u32 ARC_VOLNAME  = 0x28;
constexpr u32 ARC_INFOBLK  = 0x44;
constexpr u32 ARC_DIRS     = 0x48;
constexpr u32 ARC_STRTAB   = 0x50;

unsigned long g_allocNull = 0;
unsigned long g_allocCalls = 0;   // the denominator: "0 failures" from a hook that never fires and
                                  // "0 failures" from a healthy heap are the same line otherwise
bool g_traceWalk = false;
void watch_shortfall(const CPUState& cpu, u32 caller, u32 size);
// The system heap, captured ONCE. Reading it from r13 at every call site looked equivalent and is
// not: r13 is per-CPUState in this runtime, so a call arriving on another thread compares against
// whatever that thread's SDA slot holds — which is how the resident ledger came to claim 200,672
// bytes live in a 130,928-byte heap. One pinned pointer cannot drift.
u32 g_sysHeapAddr = 0;
u32 sys_heap(const CPUState& cpu) {
    // Cache the ADDRESS of the static, never its VALUE. Caching the value froze whatever
    // JKRHeap::sSystemHeap happened to hold the first time this ran — early in boot that is the ROOT
    // heap, and the resident ledger then attributed 20.8 MB (including a single 15.7 MB block) to a
    // 130,928-byte heap. Reading through the address every time follows the static as the game
    // reassigns it, while still not depending on r13 being the same on every thread.
    if (g_sysHeapAddr == 0) g_sysHeapAddr = (u32)cpu.gpr[13] - 0x5f30u;
    return sb_r32(g_sysHeapAddr);
}
std::unordered_map<u32, u32> g_live;   // pointer -> size, for the system heap only
std::unordered_map<u32, u32> g_liveSite;   // pointer -> the return address that allocated it
unsigned long g_liveMissedFree = 0;    // frees of pointers never seen allocated
unsigned long g_freeAllSys = 0;        // wholesale releases of the system heap
unsigned long g_newSys = 0, g_freeSys = 0, g_newSys36 = 0;
unsigned long g_expFreeSys = 0, g_expFreeAll = 0;
unsigned long g_newSysBytes = 0;
unsigned long g_sizeHist[33] = {};
std::vector<u32> g_archives;
bool g_dumped = false;
bool g_retried = false;
bool g_apparatusChecked = false;
u32 g_lastQueryEa = 0;
std::string g_lastGood;

void dump_archive(u32 arc) {
    const u32 info = sb_r32(arc + ARC_INFOBLK);
    const u32 dirs = sb_r32(arc + ARC_DIRS);
    const u32 strs = sb_r32(arc + ARC_STRTAB);
    if (info == 0 || dirs == 0 || strs == 0 || sb_ram_fast(info) == nullptr) {
        lucent::info("anmdata", "  archive 0x{:08x} (\"{}\"): info/dirs/strtab = {:#x}/{:#x}/{:#x} — "
                                "not walkable, so this says nothing about its contents.",
                     arc, guest_str(sb_r32(arc + ARC_VOLNAME)), info, dirs, strs);
        return;
    }
    const u32 numNodes = sb_r32(info + 0x00);
    const u32 numFiles = sb_r32(info + 0x08);
    // REFUSE an object that is constructed but not yet MOUNTED. Its fields are whatever the heap
    // last held, and printing them as a directory listing is worse than printing nothing — the first
    // version happily reported "2,166,314,068 directory nodes" for one. A real RARC has a handful.
    if (numNodes == 0 || numNodes > 4096 || sb_ram_fast(strs) == nullptr) {
        // PRINT THE NAME EVEN WHEN SKIPPING. The first version said only "constructed but not
        // mounted", which drops the one field that decides whether this object matters: if a
        // table-less loader is sitting in the volume list under the SAME name as a real archive,
        // findVolume can resolve to it and every lookup through that name fails while the real
        // archive sits right there holding the answer. A skip line without the name cannot show
        // that, and "skipped" then reads as "irrelevant".
        lucent::info("anmdata", "  archive 0x{:08x} volume \"{}\": no usable directory table (node "
                                "count reads {}) — constructed but not mounted, OR a loader that is "
                                "not a RARC at all.",
                     arc, guest_str(sb_r32(arc + ARC_VOLNAME)), numNodes);
        return;
    }
    lucent::info("anmdata", "  archive 0x{:08x} volume \"{}\": {} directory node(s), {} file "
                            "entrie(s)",
                 arc, guest_str(sb_r32(arc + ARC_VOLNAME)), numNodes, numFiles);
    // THE ROOT DIRECTORY'S ENTRIES, which is what findDirectory actually searches. The node list
    // below is every directory in the archive FLATTENED, so a name appearing there says nothing
    // about whether "/scene/<name>" resolves — the lookup walks the root node's file entries and
    // compares CArcName's lowercased hash, then strcmp's the stored name against that lowercased
    // query (JKRArchivePri.cpp isSameName). A stored name with an uppercase letter can therefore
    // never match anything, which is the kind of thing only this listing can show.
    {
        const u32 files = sb_r32(arc + 0x4C);
        const u32 rootNum = sb_r32(dirs + 0x08) & 0xFFFF;
        const u32 rootFirst = sb_r32(dirs + 0x0C);
        lucent::info("anmdata", "    root node holds {} entrie(s) starting at file index {}:",
                     rootNum, rootFirst);
        for (u32 i = 0; i < rootNum && i < 64; ++i) {
            const u32 fe = files + (rootFirst + i) * 0x14;
            if (sb_ram_fast(fe) == nullptr) break;
            const u32 flagsName = sb_r32(fe + 0x04);
            const bool isDir = ((flagsName >> 24) & 0x02) != 0;
            // THE HASH IS THE FIRST TEST isSameName APPLIES, and a mismatch there rejects the
            // entry before the string is ever compared — so printing the name alone can show a
            // directory that is present and still unreachable. Recompute it here exactly as
            // CArcName::store does (h = tolower(c) + h*3, u16) and print both.
            const std::string nm = guest_str(strs + (flagsName & 0xFFFFFF));
            u16 want = 0;
            for (char c : nm) {
                const int lc = (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : (unsigned char)c;
                want = (u16)(lc + want * 3);
            }
            const u16 stored = (u16)(sb_r32(fe + 0x00) & 0xFFFF);
            lucent::info("anmdata", "      {:<20} {}  hash stored {:#06x} vs computed {:#06x}{}", nm,
                         isDir ? "DIR " : "file", stored, want,
                         stored == want ? "" : "   <-- MISMATCH: this entry can never be found");
        }
    }
    for (u32 i = 0; i < numNodes && i < 256; ++i) {
        const u32 e = dirs + i * 0x10;
        if (sb_ram_fast(e) == nullptr) break;
        const u32 type = sb_r32(e + 0x00);
        const u32 nameOff = sb_r32(e + 0x04);
        const u32 num = sb_r32(e + 0x08) & 0xFFFF;
        char t[5] = {(char)(type >> 24), (char)(type >> 16), (char)(type >> 8), (char)type, 0};
        lucent::info("anmdata", "    node {:>3}  type '{}'  name \"{}\"  {} entrie(s)", i, t,
                     guest_str(strs + nameOff), num);
    }
}

// THE VOLUME LIST ITSELF, which is what findVolume walks and what the archive dump cannot see.
//
// An archive object can be intact in memory — right name, right directory table — and still be
// unreachable, because mounting is LIST MEMBERSHIP and unmounting only unlinks. The dump of
// constructed archives therefore proves nothing about what is mounted, and reading it as if it did
// is what kept stage 9 looking like a missing directory for so long.
//
// Layout from findVolume's own disassembly (0x802c3124): the list head is a static at 0x804042B4;
// each link holds the loader pointer at +0x00 and the next link at +0x0C; the loader's name is at
// +0x28. Taken from the code that does the lookup rather than from a header, so the walk cannot
// disagree with the game's.
void dump_volume_list(const char* why) {
    constexpr u32 kVolumeListHead = 0x804042B4u;
    u32 link = sb_r32(kVolumeListHead);
    lucent::info("anmdata", "JKRHeap::alloc has been called {} time(s) this run, {} returned NULL. "
                            "A zero in the FIRST number means the hook never fired and the second "
                            "number says nothing at all.",
                 g_allocCalls, g_allocNull);
    lucent::info("anmdata", "MOUNTED VOLUME LIST at the moment of: {}", why);
    int n = 0;
    while (link != 0 && n < 64 && sb_ram_fast(link) != nullptr) {
        const u32 loader = sb_r32(link);
        lucent::info("anmdata", "    volume {:>2}: loader {:#010x} name \"{}\"", n, loader,
                     loader ? guest_str(sb_r32(loader + 0x28)) : std::string("<null>"));
        link = sb_r32(link + 0x0C);
        ++n;
    }
    if (n == 0) {
        lucent::info("anmdata", "    THE LIST IS EMPTY — no volume is mounted at all, so every path "
                                "lookup fails regardless of what any archive contains.");
    }
}

// The system heap's numbers, printed wherever the archive dump is — which fires on the first
// mixed-case lookup in EVERY stage, not only where the lookup failed. That is what makes stage 8
// and stage 9 comparable at the same point in the load; a number only printed on failure can never
// say how much margin the stages that survive actually have.
// WALK THE HEAP'S OWN USED-BLOCK LIST. This is ground truth and it makes the shadow ledger's
// disagreement measurable instead of arguable: JKRExpHeap keeps mHeadUsedList at +0x7C, each
// CMemBlock carrying its size at +0x04, its group id at +0x03 and the next link at +0x0C, with the
// 0x10-byte header sitting immediately before the content it hands out. If this total agrees with
// the heap's size minus its free list, the accounting is closed; if it does not, the heap's own
// bookkeeping is what is off, and no amount of hooking allocation entry points would ever have
// found that.
void dump_used_blocks(u32 hp) {
    const u32 lo = sb_r32(hp + 0x30), hi = sb_r32(hp + 0x34);
    u32 blk = sb_r32(hp + 0x7C);
    unsigned long n = 0, bytes = 0;
    unsigned long byGroup[8] = {};
    const char* stopped = "the list ended normally";
    while (blk != 0 && n < 4096) {
        // WALK VALIDITY, checked at every step. A short walk and a corrupt list produce the same
        // small total, and the difference is the whole finding: if a link leaves the heap's own
        // range or a size is impossible, the walk must say so rather than return a tidy number.
        if (blk < lo || blk >= hi) { stopped = "A LINK POINTED OUTSIDE THE HEAP — the list is corrupt"; break; }
        if (sb_ram_fast(blk) == nullptr) { stopped = "a link pointed at unmapped memory"; break; }
        const u32 size = sb_r32(blk + 0x04);
        if (size > (hi - lo)) { stopped = "A BLOCK CLAIMED A SIZE LARGER THAN THE HEAP — the list is corrupt"; break; }
        const u8 group = (u8)((sb_r32(blk + 0x00) >> 8) & 0xFF);
        bytes += size + 0x10;   // the header is part of what the block occupies
        ++byGroup[group & 7];
        ++n;
        blk = sb_r32(blk + 0x0C);
    }
    // The FREE list too, so the two totals can be added up against the heap's size. Memory in
    // neither list is memory the heap has LOST, which is a different fault from memory in use.
    unsigned long freeN = 0, freeBytes = 0;
    for (u32 f = sb_r32(hp + 0x74); f != 0 && freeN < 4096 && f >= lo && f < hi; f = sb_r32(f + 0x0C)) {
        freeBytes += sb_r32(f + 0x04) + 0x10;
        ++freeN;
    }
    lucent::info("anmdata", "  heap accounting: used list {} bytes + free list {} bytes ({} free "
                            "block(s)) = {} of {}. A shortfall is memory in NEITHER list — lost, not "
                            "in use. Walk stopped because: {}",
                 bytes, freeBytes, freeN, bytes + freeBytes, sb_r32(hp + 0x38), stopped);
    lucent::Line l;
    l.add("  heap used-block list: {} block(s) occupying {} bytes (content + 0x10 header each) of a "
          "{} byte heap. By group id:", n, bytes, sb_r32(hp + 0x38));
    for (int g = 0; g < 8; ++g) {
        if (byGroup[g]) l.add(" {}:{}", g, byGroup[g]);
    }
    l.flush(lucent::Level::Info, "anmdata");
}

void dump_sys_heap(const CPUState& cpu) {
    const u32 hp = sys_heap(cpu);
    if (hp == 0) return;
    CPUState a = cpu, b = cpu;
    a.gpr[3] = hp;
    func_802c1edc(a);
    b.gpr[3] = hp;
    func_802c1f48(b);
    lucent::info("anmdata", "SYSTEM HEAP {:#010x}: largest free block {} bytes, TOTAL free {} bytes, "
                            "of a {} byte heap. Four JKRThread stacks (JUTException, JKRAram, "
                            "JKRAramStream, JKRDecomp) take 16 KB each in every stage, so 64 KB of "
                            "this is structural.",
                 hp, (s32)a.gpr[3], (s32)b.gpr[3], sb_r32(hp + 0x38));
    dump_used_blocks(hp);
}

void dump_all_archives(const char* why) {
    if (g_dumped) return;   // once: the answer does not change and the list is long
    g_dumped = true;
    lucent::info("anmdata", "MOUNTED ARCHIVES at the moment of: {}  ({} archive(s) constructed this "
                            "run){}",
                 why, g_archives.size(),
                 g_archives.empty()
                     ? "   <-- NONE were recorded, so this listing proves nothing about what is "
                       "mounted; the constructor hook did not fire."
                     : "");
    for (u32 a : g_archives) dump_archive(a);
    dump_volume_list(why);
}

// THE ONE THAT MATTERS. MActorAnmData::init crashes when this returns null, and it returns null for
// a directory no mounted archive contains. Logging the path with its result turns "null pointer in
// an animation loader" into "this directory is not there" — and logging the SUCCESSES too is what
// makes the failure readable, because a lone failure line cannot say whether the archive is missing
// entirely or missing one directory.
void ov_find_first_file(CPUState& cpu) {
    const std::string path = guest_str((u32)cpu.gpr[3]);
    g_lastQueryEa = (u32)cpu.gpr[3];
    // Sample the heap accounting from HERE too. The allocation-side watch reported only two 12-byte
    // rounding changes on a heap that ends 79 KB short, which means the loss happens between the
    // operations it can see. findFirstFile is called throughout the stage load and is not an
    // allocator, so it brackets those gaps.
    watch_shortfall(cpu, (u32)cpu.lr, 0);
    func_802c31b0(cpu);
    const u32 res = (u32)cpu.gpr[3];
    lucent::debug("anmdata", "findFirstFile(\"{}\") -> {}", path,
                  res == 0 ? std::string("NULL  <-- no mounted archive has this directory")
                           : std::string("ok"));
    // VALIDATE THE APPARATUS BEFORE TRUSTING IT. The retry below calls the guest's findFirstFile
    // from inside this override with a copied CPUState. If that calling convention were wrong, the
    // retry would return null for every input and would look exactly like the failure it is trying
    // to explain — the "instrument that cannot fail" trap. So the FIRST time a lookup SUCCEEDS, the
    // same machinery repeats it: that call MUST come back ok, and if it does not, nothing else this
    // probe says about retries means anything.
    if (res != 0 && !g_apparatusChecked) {
        g_apparatusChecked = true;
        CPUState chk = cpu;
        chk.gpr[3] = g_lastQueryEa;
        func_802c31b0(chk);
        lucent::info("anmdata",
                     "APPARATUS CHECK: repeating the just-SUCCEEDED lookup \"{}\" through the same "
                     "call path -> {}. This must read ok; a NULL here means the retry mechanism is "
                     "broken and every conclusion drawn from a retry is void.",
                     path, (u32)chk.gpr[3] == 0 ? "NULL  <-- BROKEN" : "ok");
    }

    // MIXED CASE IS THE WHOLE QUESTION HERE, so it gets its own line and its own dump.
    //
    // JKRArchive lookups are supposed to be case-insensitive: CArcName::store lowercases every
    // character into both the hash and the stored string (JKRArchivePri.cpp:189), and isSameName
    // then compares that lowercased query against the archive's own string table. Stage 9's
    // "/scene/mapObj" fails while the scene archive demonstrably holds a root entry spelled
    // "mapobj" — which can only happen if the lowercasing did not happen. Stage 8 runs the SAME
    // query successfully, so dumping on the first mixed-case lookup in EITHER stage is what
    // compares the two, rather than dumping only where it already failed.
    bool mixedCase = false;
    for (char c : path) {
        if (c >= 'A' && c <= 'Z') { mixedCase = true; break; }
    }
    if (res != 0) g_lastGood = path;
    if (mixedCase) {
        dump_sys_heap(cpu);
        lucent::debug("anmdata", "  ^ that query contains UPPERCASE. JKR lookups lowercase the "
                                 "query before hashing, so it must match a lowercase-spelled entry; "
                                 "result was {}.",
                      res == 0 ? "NULL" : "ok");
        dump_all_archives(path.c_str());
    }
    // THE EXPERIMENT THAT SEPARATES THE LAST TWO EXPLANATIONS. Everything static checks out: at the
    // moment "/scene/mapObj" returns null, the scene archive is mounted, its root holds a DIR entry
    // spelled "mapobj", and that entry's stored hash equals the one CArcName computes. So either the
    // query's lowercasing is not happening (and only a lowercase query can match), or the failure is
    // somewhere else entirely. Asking the guest's own findFirstFile the SAME question in lowercase
    // answers that in one call.
    //
    // The path is lowercased IN PLACE and restored immediately, so the guest sees its own buffer
    // unchanged; the retry runs the real function, so it is the game's answer and not a
    // reimplementation of the lookup that could differ from it.
    if (res == 0 && mixedCase && !g_retried) {
        g_retried = true;
        const u32 ea = (u32)cpu.gpr[3] != 0 ? (u32)cpu.gpr[3] : 0;
        (void)ea;
        std::string lower = path;
        for (char& c : lower) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        const u32 buf = g_lastQueryEa;
        g_traceWalk = true;
        if (buf != 0) {
            for (size_t i = 0; i < lower.size(); ++i) sb_w8(buf + (u32)i, (u8)lower[i]);
            CPUState retry = cpu;
            retry.gpr[3] = buf;
            func_802c31b0(retry);
            const u32 r2 = (u32)retry.gpr[3];
            for (size_t i = 0; i < path.size(); ++i) sb_w8(buf + (u32)i, (u8)path[i]);
            lucent::info("anmdata",
                         "RETRY of the same lookup in lowercase (\"{}\") -> {}. If lowercase "
                         "SUCCEEDS where mixed case failed, the query is not being lowercased and "
                         "the fault is in tolower/CArcName; if it fails too, the case is innocent "
                         "and the fault is in the volume or directory walk.",
                         lower, r2 == 0 ? "NULL" : "ok");
            // THE CONTROL. Ask for a directory that RESOLVED EARLIER IN THIS RUN, right now. If it
            // answers, the lookup machinery is working at this instant and the fault is specific to
            // this path; if it also returns null, nothing is resolvable at this moment and the fault
            // is not about paths at all (an exhausted heap cannot allocate the finder, a volume list
            // that has been torn down, and so on). Without this control, "path X fails" is
            // indistinguishable from "everything fails".
            // THE CONTROL STRING MUST BE ONE THIS RUN ACTUALLY RESOLVED, not one that ought to.
            // The first version used "/scene/map" and called it "resolved earlier in this very run";
            // it was not — what resolved was "/scene/map/map". A control that was never observed to
            // succeed cannot distinguish "this path fails" from "that path was never valid", and it
            // read as damning evidence for a whole round of investigation. The last successful query
            // is now REMEMBERED and replayed verbatim.
            const std::string known = g_lastGood;
            for (u32 i = 0; i < (u32)known.size(); ++i) sb_w8(buf + i, (u8)known[i]);
            sb_w8(buf + (u32)known.size(), 0);
            CPUState ctl = cpu;
            ctl.gpr[3] = buf;
            func_802c31b0(ctl);
            const u32 r3 = (u32)ctl.gpr[3];
            for (size_t i = 0; i < path.size(); ++i) sb_w8(buf + (u32)i, (u8)path[i]);
            sb_w8(buf + (u32)path.size(), 0);
            // WHERE does it fail — picking the volume, or walking it? findVolume is the first half
            // of findFirstFile and is callable on its own. Scratch lives below the guest stack
            // pointer (unused space) so nothing live is disturbed: the string goes at +0x10 and the
            // pointer-to-pointer findVolume expects at +0x00.
            {
                const u32 scratch = ((u32)cpu.gpr[1] - 0x400u) & ~3u;
                for (u32 i = 0; i < (u32)known.size(); ++i) sb_w8(scratch + 0x10 + i, (u8)known[i]);
                sb_w8(scratch + 0x10 + (u32)known.size(), 0);
                sb_w32(scratch, scratch + 0x10);
                CPUState fv = cpu;
                fv.gpr[3] = scratch;
                func_802c3124(fv);
                const u32 vol = (u32)fv.gpr[3];
                lucent::info("anmdata",
                             "  findVolume(\"{}\") at the same instant -> {:#010x} name \"{}\". A "
                             "volume HERE with a null lookup above puts the fault in the archive's "
                             "directory walk; a null here puts it in volume resolution.",
                             known, vol,
                             vol ? guest_str(sb_r32(vol + 0x28)) : std::string("<none>"));
            }
            // sCurrentDirID IS THE PRIME SUSPECT and it is one word away.
            //
            // JKRArchive::getFirstFile (JKRArchivePub.cpp:269) only searches from the ROOT when the
            // path it receives starts with '/'. findVolume CONSUMES the volume component, so what
            // getFirstFile actually gets is "map/map" or "mapObj" — no leading slash — and those go
            // to `findDirectory(path, sCurrentDirID)`, a STATIC current-directory id set by
            // becomeCurrent. If that id is left pointing at some subdirectory, every relative lookup
            // fails no matter what it asks for, which is exactly the path-independent failure the
            // control measured.
            //
            // Addresses read from becomeCurrent's own disassembly (0x802bee30): it writes the
            // current volume to r13-0x5f38 and the current dir id to r13-0x5f98.
            {
                const u32 sda = (u32)cpu.gpr[13];
                const u32 curVol = sb_r32(sda - 0x5f38u);
                const u32 curDir = sb_r32(sda - 0x5f98u);
                lucent::info("anmdata",
                             "  sSystemHeap = {:#010x}, sCurrentVolume = {:#010x} (\"{}\"), "
                             "sCurrentDirID = {}. getFirstFile "
                             "searches from THIS directory for any path without a leading slash, and "
                             "findVolume strips the volume component before handing the rest over — "
                             "so a non-zero id here means root is not being searched.",
                             sb_r32(sda - 0x5f30u), curVol,
                             curVol ? guest_str(sb_r32(curVol + 0x28)) : std::string("<none>"),
                             (s32)curDir);
            }
            // HOW MUCH IS LEFT IN IT. A 36-byte allocation failing says either "exhausted" or "this
            // heap object is broken", and the free size separates them: a heap with megabytes free
            // that refuses 36 bytes is corrupt, one with nothing free is simply full.
            {
                CPUState fs = cpu;
                fs.gpr[3] = sb_r32((u32)cpu.gpr[13] - 0x5f30u);
                func_802c1edc(fs);
                const u32 hp = sb_r32((u32)cpu.gpr[13] - 0x5f30u);
                // BOTH NUMBERS, because they answer different questions and the names invite the
                // wrong one. getFreeSize returns the LARGEST FREE BLOCK (JKRExpHeap.cpp:570 walks
                // the free list keeping the maximum); getTotalFreeSize sums them. A large total with
                // a tiny maximum is FRAGMENTATION; both tiny is exhaustion. Reading getFreeSize as
                // "free memory" would have made a fragmented heap look full.
                CPUState tf = cpu;
                tf.gpr[3] = hp;
                func_802c1f48(tf);
                lucent::info("anmdata",
                             "  sSystemHeap TOTAL free = {} bytes across the whole free list, while "
                             "the LARGEST single free block is the number below. Both small means "
                             "exhausted; a big total with a small largest means fragmented.",
                             (s32)tf.gpr[3]);
                lucent::info("anmdata",
                             "  sSystemHeap {:#010x}: free {} bytes of a {} byte heap ({:#010x}..{:#010x}). "
                             "The allocation that failed asked for 36. A heap this full is a MEMORY "
                             "BUDGET fault in the port, not a JKR fault: the archive lookup it broke "
                             "had already found its directory.",
                             hp, (s32)fs.gpr[3], sb_r32(hp + 0x38), sb_r32(hp + 0x30),
                             sb_r32(hp + 0x34));
            }
            {
                // FILTER BY THE HEAP'S OWN ADDRESS RANGE, not by which heap the allocation was
                // charged to. sSystemHeap is REASSIGNED during boot — it starts out as the root heap
                // and is later pointed at a 128 KB child — so a ledger keyed on "the heap was
                // sSystemHeap at the time" mixes two epochs and listed the 131,072-byte block that
                // CREATED the system heap as if it were inside it. An address range cannot be
                // confused that way: a pointer either lies in [mStart, mEnd) or it does not.
                const u32 hp2 = sys_heap(cpu);
                const u32 lo = sb_r32(hp2 + 0x30), hi = sb_r32(hp2 + 0x34);
                std::vector<std::pair<u32, u32>> big;
                for (const auto& kv : g_live) {
                    if (kv.first >= lo && kv.first < hi) big.push_back(kv);
                }
                std::sort(big.begin(), big.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
                unsigned long bytes = 0;
                unsigned long hist[33] = {};
                for (const auto& kv : big) {
                    bytes += kv.second;
                    ++hist[kv.second < 1024 ? (kv.second / 32) : 32];
                }
                for (size_t i = 0; i < big.size() && i < 5; ++i) {
                    const auto siteIt = g_liveSite.find(big[i].first);
                    const u32 site = siteIt == g_liveSite.end() ? 0u : siteIt->second;
                    lucent::info("anmdata", "    largest live block #{}: {} bytes at {:#010x}, "
                                            "allocated from {:#010x} ({})",
                                 i + 1, big[i].second, big[i].first, site,
                                 site ? sbr_gfxdb_symbolize(site) : std::string("<unknown>"));
                }
                lucent::Line l;
                l.add("  system heap RESIDENT at failure: {} live allocation(s) inside "
                      "{:#010x}..{:#010x} holding {} bytes of {}. Sizes:", big.size(), lo, hi, bytes,
                      sb_r32(hp2 + 0x38));
                for (int i = 0; i < 33; ++i) {
                    if (hist[i] == 0) continue;
                    if (i == 32) l.add(" 1024+:{}", hist[i]);
                    else l.add(" {}-{}:{}", i * 32, i * 32 + 31, hist[i]);
                }
                l.add("  ({} free(s) were of pointers this probe never saw allocated — allocations "
                      "predating it, NOT counted above; {} wholesale freeAll(s) cleared the ledger)",
                      g_liveMissedFree, g_freeAllSys);
                l.flush(lucent::Level::Info, "anmdata");
            }
            lucent::info("anmdata",
                         "  system-heap traffic so far: {} allocation(s) totalling {} bytes ({} of "
                         "them 36 bytes, the finder size) against {} free(s) via JKRHeap::free and "
                         "{} via JKRExpHeap::free ({} frees on all heaps). A large gap is a LEAK; a "
                         "small one means the heap is legitimately full and the budget is the "
                         "problem.",
                         g_newSys, g_newSysBytes, g_newSys36, g_freeSys, g_expFreeSys, g_expFreeAll);
            {
                lucent::Line l;
                l.add("  system-heap allocation sizes (32-byte buckets):");
                for (int i = 0; i < 33; ++i) {
                    if (g_sizeHist[i] != 0) {
                        if (i == 32) l.add(" 1024+:{}", g_sizeHist[i]);
                        else l.add(" {}-{}:{}", i * 32, i * 32 + 31, g_sizeHist[i]);
                    }
                }
                l.flush(lucent::Level::Info, "anmdata");
            }
            lucent::info("anmdata",
                         "CONTROL lookup of \"{}\" at the same instant -> {}. That exact string "
                         "resolved earlier in THIS run, so a NULL here means the failure is not "
                         "about the path being asked for.",
                         known, r3 == 0 ? "NULL" : "ok");
        }
    }
    g_traceWalk = false;
    if (res == 0) dump_all_archives(path.c_str());
}

// BOTH base constructors, because hooking one of them is how a listing lies while looking complete.
// The first version caught only JKRArchive(s32, EMountMode) and reported "2 archives" for a run in
// which /scene/map/map had already resolved successfully — i.e. it was missing the very archive the
// question was about. A derived archive (Mem/Aram/Dvd/Comp) runs exactly one of these two.
void ov_archive_ctor(CPUState& cpu) {
    const u32 self = (u32)cpu.gpr[3];
    func_802bc9f8(cpu);
    g_archives.push_back(self);
}

// WHICH VOLUME does a path resolve to? findFirstFile picks a mounted loader by the path's first
// component, and every explanation left for stage 9 turns on that choice: the scene ARCHIVE holds a
// root entry "mapobj" with a correct hash at the moment the lookup returns null, so either the
// lookup is searching a different volume that also answers to "scene", or it is searching the right
// one and failing inside. Printing the resolved volume separates those two in one run.
// A NULL FROM THE ALLOCATOR IS A SILENT FAILURE EVERYWHERE IT HAPPENS. JKRHeap::alloc returns null
// on exhaustion and the game's own code mostly does not check — which is how "findFirstFile returned
// null" and "the finder could not be allocated" become the same observable. Counting them (and
// naming the first few sizes) turns an unexplained null three layers up into a heap that is full.
void ov_heap_alloc(CPUState& cpu) {
    const u32 size = (u32)cpu.gpr[3];
    ++g_allocCalls;
    const u32 wrapperCaller = (u32)cpu.lr;
    func_802c3740(cpu);
    // OVERWRITE the site recorded by the inner JKRExpHeap::alloc hook. That one only ever sees this
    // wrapper as its caller (alloc__7JKRHeapFUliP7JKRHeap+0x3c for every block), which names the
    // allocator and not the system doing the allocating — one frame short of the answer.
    if ((u32)cpu.gpr[3] != 0) g_liveSite[(u32)cpu.gpr[3]] = wrapperCaller;
    watch_shortfall(cpu, wrapperCaller, size);
    if ((u32)cpu.gpr[3] == 0) {
        ++g_allocNull;
        if (g_allocNull <= 8) {
            lucent::info("anmdata", "JKRHeap::alloc({} bytes) returned NULL — allocation #{} to fail "
                                    "this run. The caller almost certainly does not check.",
                         size, g_allocNull);
        }
    }
}

// THE INNERMOST TWO STEPS. Everything outside them has now been measured and is healthy — the
// volume resolves, the archive's tables are intact and hold the entry with a matching hash, the
// allocator is not failing. So the next thing to observe is the walk itself: what path and which
// starting directory findDirectory is given, and whether getFirstFile turns a found directory into
// a finder. Logged only around a failure, so a healthy run stays quiet.
void ov_get_first_file(CPUState& cpu) {
    const std::string path = g_traceWalk ? guest_str((u32)cpu.gpr[4]) : std::string();
    const u32 self = (u32)cpu.gpr[3];
    func_802bf41c(cpu);
    if (g_traceWalk) {
        lucent::info("anmdata", "    JKRArchive::getFirstFile(archive {:#010x}, \"{}\") -> {:#010x}",
                     self, path, (u32)cpu.gpr[3]);
    }
}

void ov_find_directory(CPUState& cpu) {
    const std::string path = g_traceWalk ? guest_str((u32)cpu.gpr[4]) : std::string();
    const u32 dirId = (u32)cpu.gpr[5];
    func_802bcb14(cpu);
    if (g_traceWalk) {
        lucent::info("anmdata", "    JKRArchive::findDirectory(\"{}\", dirId {}) -> {:#010x}", path,
                     dirId, (u32)cpu.gpr[3]);
    }
}

// THE LAST HOP. getFirstFile's disassembly (0x802bf47c) allocates its finder with
// `operator new(0x24, sSystemHeap, 0)` — a DIFFERENT entry point from the JKRHeap::alloc this file
// was already watching, which is why that hook reported 1,011 calls and no failures while the
// allocation that actually matters was failing unobserved. Watching one allocator and concluding
// "allocation is fine" is the same error as watching one call site and concluding "nothing else
// draws cubes".
// A 128 KB system heap with 8 bytes left, and the allocation that fails is a 36-byte FINDER — so
// the question is whether finders are being freed. Counting both sides is the only way to tell a
// leak from a heap that is legitimately full: "the heap is full" is a symptom of either.
void ov_op_new_heap(CPUState& cpu) {
    const u32 size = (u32)cpu.gpr[3];
    const u32 heap = (u32)cpu.gpr[4];
    if (heap == sys_heap(cpu)) {
        ++g_newSys;
        g_newSysBytes += size;
        if (size == 36) ++g_newSys36;
        // WHAT is filling it. A size histogram names the object class without needing a symbol for
        // every caller: 36 is the finder, and whatever dominates here is the actual budget.
        ++g_sizeHist[size < 1024 ? (size / 32) : 32];
    }
    func_802c3c38(cpu);
    if ((u32)cpu.gpr[3] == 0 && g_traceWalk) {
        lucent::info("anmdata", "    operator new({} bytes, heap {:#010x}) -> NULL  <-- this is what "
                                "makes getFirstFile return null on a directory it FOUND",
                     size, heap);
    }
}

// WHAT IS RESIDENT IN THE SYSTEM HEAP, which is the question left after "it is exhausted". Traffic
// counts cannot answer it — 710 allocations against 708 frees says the flow balances and says
// nothing about the 128 KB that never left. So every allocation from this heap is remembered by
// pointer and forgotten on free; what remains at the moment of failure IS the occupancy, by size.
//
// Hooked at JKRExpHeap::alloc rather than at one of the wrappers: everything that reaches this heap
// passes through it, and this file has already been burned twice by watching one of several entry
// points and reading the resulting zero as an answer.
void ov_exp_alloc(CPUState& cpu) {
    const u32 heap = (u32)cpu.gpr[3];
    const u32 size = (u32)cpu.gpr[4];
    func_802c138c(cpu);
    const u32 ptr = (u32)cpu.gpr[3];
    if (ptr != 0) {
        g_live[ptr] = size;
        // WHO asked. A size on its own names a shape, not a system — and the four 16 KB blocks that
        // dominate this heap could be anything until their caller is named. The return address is
        // symbolized through the graphics registry's resolver, which bounds addresses by the
        // recompiler's own function table and so reports an unnamed function honestly instead of as
        // `some_symbol+0x4000`.
        g_liveSite[ptr] = (u32)cpu.lr;
    }
}

// BULK FREES MUST CLEAR THE LEDGER. Without this the resident set only ever grows: freeAll releases
// every block at once without a per-pointer free, so the map keeps entries for memory that is gone
// and the total climbs past the heap's own size. The first run of this probe reported 200,672 bytes
// resident in a 130,928-byte heap — an arithmetic impossibility, which is the useful kind of wrong
// answer because it cannot be mistaken for a finding.
void ov_exp_free_all(CPUState& cpu) {
    if ((u32)cpu.gpr[3] == sys_heap(cpu)) {
        g_live.clear();
        ++g_freeAllSys;
    }
    func_802c1b88(cpu);
}

// EVERY JKRThread TAKES ITS STACK FROM THE SYSTEM HEAP, and that is where the 128 KB goes: four
// live 16 KB stacks plus their thread objects account for 65 KB of it. Whether that is normal or a
// port defect turns on the COUNT and the SIZE, so both are logged with the caller — a stage that
// creates more threads than another, or one that creates them per load without destroying them,
// shows up here immediately.
unsigned long g_threads = 0;
struct ThreadStack { u32 base, size, creator; };
std::vector<ThreadStack> g_threadStacks;

void ov_jkr_thread_ctor(CPUState& cpu) {
    ++g_threads;
    const u32 size = (u32)cpu.gpr[4];
    const u32 caller = (u32)cpu.lr;
    func_802c52b0(cpu);
    // RECORD THE STACK'S ADDRESS RANGE, not just its size. The recursion that overflows runs with a
    // stack pointer somewhere in one of these blocks, and "which thread is this running on" is the
    // question that decides whether the recursion is too deep or simply on the wrong thread.
    // The block address is the most recent system-heap allocation of exactly `size` bytes.
    u32 blk = 0;
    for (const auto& kv : g_live) {
        if (kv.second == size && kv.first > blk) blk = kv.first;
    }
    g_threadStacks.push_back({blk, size, caller});
    lucent::info("anmdata", "JKRThread #{}: stack {} bytes at {:#010x}..{:#010x}, created by {}",
                 g_threads, size, blk, blk + size, sbr_gfxdb_symbolize(caller));
}

// THE TWO PATHS JKRExpHeap::alloc DELEGATES TO. The ledger built on ::alloc accounted for 69,600
// bytes of a heap with 130,912 in use, and a probe that can only see half the allocations will
// happily present the half it sees as the whole. allocFromHead/allocFromTail are where the memory
// is actually taken, so recording there closes the gap — and if it does not close, the remainder is
// a third path and the number stays honest about that.
void record_live(const CPUState& cpu, u32 heapThis, u32 size, u32 ptr, u32 caller) {
    (void)cpu;
    (void)heapThis;
    // RECORD EVERY HEAP, filter by address range at report time. Recording only allocations charged
    // to sSystemHeap missed every block taken from this heap object BEFORE the static was pointed at
    // it — i.e. the boot-time occupancy, which is most of it. The ledger accounted for 69,600 bytes
    // of a heap with 130,912 in use, and a probe that sees half the memory will present that half as
    // the whole unless the denominator is checked. It is checked: the report prints resident bytes
    // against the heap's own size, and the two should now agree.
    if (ptr == 0) return;
    g_live[ptr] = size;
    if (g_liveSite.find(ptr) == g_liveSite.end()) g_liveSite[ptr] = caller;
}

// WATCH THE SHORTFALL GROW. The two lists are short (tens of blocks), so recomputing the accounting
// after every system-heap allocation costs nothing measurable — and a shortfall that appears
// between two allocations names the one that did it, which no end-of-run total can.
long g_lastShortfall = 0;
bool g_shortfallSeen = false;
unsigned long g_shortfallChanges = 0;
std::vector<u32> g_prevChain;

void watch_shortfall(const CPUState& cpu, u32 caller, u32 size) {
    const u32 hp = sys_heap(cpu);
    if (hp == 0) return;
    const u32 lo = sb_r32(hp + 0x30), hi = sb_r32(hp + 0x34);
    if (lo == 0 || hi <= lo) return;
    unsigned long used = 0, freeb = 0;
    int n = 0;
    // Keep the CHAIN, not just its total. When the shortfall jumps, the useful question is not how
    // much vanished but WHERE the list stops agreeing with itself a moment earlier — the last block
    // still common to both walks is the one whose link was overwritten, and its header address is
    // what a watchpoint should be pointed at.
    std::vector<u32> chain;
    for (u32 b = sb_r32(hp + 0x7C); b != 0 && b >= lo && b < hi && n < 4096; b = sb_r32(b + 0x0C)) {
        used += sb_r32(b + 0x04) + 0x10;
        chain.push_back(b);
        ++n;
    }
    n = 0;
    for (u32 f = sb_r32(hp + 0x74); f != 0 && f >= lo && f < hi && n < 4096; f = sb_r32(f + 0x0C)) {
        freeb += sb_r32(f + 0x04) + 0x10;
        ++n;
    }
    const long shortfall = (long)sb_r32(hp + 0x38) - (long)(used + freeb);
    // THE FIRST SAMPLE MUST BE PRINTED, whatever it is. A watcher that only reports GROWTH cannot
    // report a shortfall that was already present when it started looking — it would sit silent
    // through exactly the case it was built for, which is what happened on the first two runs of
    // this: no output at all, from a heap that was 79 KB short.
    if (!g_shortfallSeen) {
        g_shortfallSeen = true;
        g_lastShortfall = shortfall;
        lucent::info("anmdata", "SYSTEM HEAP baseline: shortfall is {} bytes at the first sample "
                                "(caller {}). Anything already missing here happened before this "
                                "watch began.",
                     shortfall, sbr_gfxdb_symbolize(caller));
        return;
    }
    // ANY change, not a 4 KB one. The first version required growth over 4,096 bytes and reported
    // nothing at all on a heap that ends up 79 KB short — the loss arrives in small steps, and a
    // threshold picked for readability hid every one of them. Capped at 12 lines so a steady drip
    // does not drown the log, with the count of suppressed changes reported at the end.
    if (shortfall - g_lastShortfall > 4096) {
        // The diff that names the broken link. Walk both chains together; the first position where
        // they diverge is where the list was cut, and the block BEFORE it still holds the link that
        // was overwritten.
        size_t i = 0;
        while (i < chain.size() && i < g_prevChain.size() && chain[i] == g_prevChain[i]) ++i;
        if (i > 0 && i <= g_prevChain.size()) {
            const u32 lastGood = chain.empty() ? 0 : chain[i - 1];
            lucent::warn("anmdata",
                         "USED LIST CUT: the chain agreed for {} block(s) and then diverged. Last "
                         "common block {:#010x}; its next link now reads {:#010x}, previously "
                         "{:#010x}. The word at {:#010x} is what to watch: run again with "
                         "SBR_WATCH=0x{:08x} and the store that overwrites it will print "
                         "its own guest stack.",
                         i, lastGood, lastGood ? sb_r32(lastGood + 0x0C) : 0,
                         i < g_prevChain.size() ? g_prevChain[i] : 0, lastGood + 0x0C,
                         lastGood + 0x0C);
        } else {
            lucent::warn("anmdata",
                         "USED LIST CUT at the very head: the previous chain had {} block(s), this "
                         "one has {}, and they share no prefix. mHeadUsedList itself ({:#010x}) is "
                         "what to watch.",
                         g_prevChain.size(), chain.size(), hp + 0x7C);
        }
    }
    g_prevChain.swap(chain);
    if (shortfall != g_lastShortfall) {
        ++g_shortfallChanges;
        if (g_shortfallChanges <= 12) {
            lucent::info("anmdata",
                         "SYSTEM HEAP SHORTFALL {} -> {} bytes (change #{}) across a {}-byte "
                         "operation from {} ({}).",
                         g_lastShortfall, shortfall, g_shortfallChanges, size, caller,
                         sbr_gfxdb_symbolize(caller));
        }
    }
    if (shortfall != g_lastShortfall) g_lastShortfall = shortfall;
}

void ov_alloc_head(CPUState& cpu) {
    const u32 heap = (u32)cpu.gpr[3], size = (u32)cpu.gpr[4], caller = (u32)cpu.lr;
    func_802c14d0(cpu);
    record_live(cpu, heap, size, (u32)cpu.gpr[3], caller);
    if (heap == sys_heap(cpu)) watch_shortfall(cpu, caller, size);
}

void ov_alloc_tail(CPUState& cpu) {
    const u32 heap = (u32)cpu.gpr[3], size = (u32)cpu.gpr[4], caller = (u32)cpu.lr;
    func_802c18dc(cpu);
    record_live(cpu, heap, size, (u32)cpu.gpr[3], caller);
    if (heap == sys_heap(cpu)) watch_shortfall(cpu, caller, size);
}

// AND THE DEFAULT-ALIGNMENT OVERLOADS. Four entry points, not two: the ledger still showed 69,600
// bytes in a heap with 130,912 in use after hooking the two-argument forms, and a gap that size is
// not rounding. Hooking a subset of an overload set and reading the total as complete is the same
// error this file has now made three times with three different functions.
void ov_alloc_head1(CPUState& cpu) {
    const u32 heap = (u32)cpu.gpr[3], size = (u32)cpu.gpr[4], caller = (u32)cpu.lr;
    func_802c17b0(cpu);
    record_live(cpu, heap, size, (u32)cpu.gpr[3], caller);
}

void ov_alloc_tail1(CPUState& cpu) {
    const u32 heap = (u32)cpu.gpr[3], size = (u32)cpu.gpr[4], caller = (u32)cpu.lr;
    func_802c1a34(cpu);
    record_live(cpu, heap, size, (u32)cpu.gpr[3], caller);
}

// IS IT REALLY RECURSING? The watchpoint's stack dump showed 54 identical frames, and that dump was
// taken from a stack that is by then demonstrably corrupt — a walker following a smashed LR chain
// prints exactly this. Counting entries and exits settles it without trusting the walk, and the
// guest stack pointer at each level says how much stack a level actually costs.
unsigned long g_deepDepth = 0, g_deepMax = 0;
u32 g_deepSpTop = 0, g_deepSpLow = 0xFFFFFFFFu;

void ov_deep_fn(CPUState& cpu) {
    ++g_deepDepth;
    const u32 sp = (u32)cpu.gpr[1];
    if (g_deepDepth == 1) {
        g_deepSpTop = sp;
        // Name the stack this recursion is running on, once. A 16 KB JKR worker stack and the main
        // game stack are the difference between "this recursion is too deep" and "this work is on
        // the wrong thread", and the depth alone cannot tell them apart.
        static bool once = false;
        if (!once) {
            once = true;
            const char* whose = "NOT any JKRThread stack (the main thread, or a stack this probe "
                                "did not see created)";
            std::string detail;
            for (const auto& t : g_threadStacks) {
                if (sp >= t.base && sp < t.base + t.size) {
                    detail = sbr_gfxdb_symbolize(t.creator);
                    whose = detail.c_str();
                    lucent::info("anmdata", "the recursion runs on the JKRThread created by {} — a {} "
                                            "byte stack at {:#010x}..{:#010x}, with the stack pointer "
                                            "already at {:#010x} ({} bytes used before the recursion "
                                            "even starts)",
                                 detail, t.size, t.base, t.base + t.size, sp,
                                 t.base + t.size - sp);
                    break;
                }
            }
            // AND THE PATH THAT GOT HERE. Which thread is only half the question; the other half is
            // what put model calc on it. Dumped once, at the first entry, while the stack is still
            // intact — the watchpoint's dump was taken after the overflow, when the frame chain can
            // no longer be trusted.
            rt_dump_guest_stack("first entry to the J3D calc recursion");
            if (detail.empty()) {
                lucent::info("anmdata", "the recursion runs on a stack at {:#010x} which is {}", sp,
                             whose);
            }
        }
    }
    if (sp < g_deepSpLow) g_deepSpLow = sp;
    if (g_deepDepth > g_deepMax) {
        g_deepMax = g_deepDepth;
        // Powers of two all the way up: the question is whether this terminates at a plausible
        // scene-graph depth or runs away, and a threshold list that stops at 128 cannot tell.
        if ((g_deepMax & (g_deepMax - 1)) == 0) {
            lucent::info("anmdata", "0x802d4cf0 recursion depth reached {} — stack from {:#010x} down "
                                    "to {:#010x} ({} bytes used)",
                         g_deepMax, g_deepSpTop, sp, g_deepSpTop - sp);
        }
    }
    func_802d4cf0(cpu);
    --g_deepDepth;
}

void ov_heap_free(CPUState& cpu) {
    if ((u32)cpu.gpr[4] == sys_heap(cpu)) ++g_freeSys;
    func_802c37b8(cpu);
}

// The virtual free, which is where a `delete` on an ExpHeap object lands. Hooking only the static
// JKRHeap::free reported ZERO frees, and "zero frees" from an unhooked path and "zero frees"
// because nothing is freed are the same number — the exact trap this file has already fallen into
// once today with the allocator.
void ov_exp_free(CPUState& cpu) {
    if ((u32)cpu.gpr[3] == sys_heap(cpu)) ++g_expFreeSys;
    ++g_expFreeAll;
    const u32 ptr = (u32)cpu.gpr[4];
    const auto it = g_live.find(ptr);
    if (it != g_live.end()) { g_live.erase(it); g_liveSite.erase(ptr); }
    else if (ptr != 0) ++g_liveMissedFree;
    func_802c1b14(cpu);
    // FREES TOO. No shortfall growth was seen across allocations, which leaves the other side of the
    // ledger: a free that unlinks a block from the used list without returning it to the free list
    // loses exactly this way, and would be invisible to an allocation-only watch.
    if ((u32)cpu.gpr[3] == sys_heap(cpu)) watch_shortfall(cpu, (u32)cpu.lr, 0);
}

void ov_find_volume(CPUState& cpu) {
    const u32 pp = (u32)cpu.gpr[3];
    const std::string want = (pp != 0 && sb_ram_fast(pp)) ? guest_str(sb_r32(pp)) : std::string("<?>");
    func_802c3124(cpu);
    const u32 vol = (u32)cpu.gpr[3];
    lucent::debug("anmdata", "findVolume(\"{}\") -> {:#010x} name \"{}\" type {:#x}", want, vol,
                  vol ? guest_str(sb_r32(vol + 0x28)) : std::string("<none>"),
                  vol ? sb_r32(vol + 0x2C) : 0);
}

void ov_archive_ctor_default(CPUState& cpu) {
    const u32 self = (u32)cpu.gpr[3];
    func_802bc9ac(cpu);
    g_archives.push_back(self);
}

} // namespace

SB_OVERRIDE(0x802c17b0u, ov_alloc_head1, "JKRExpHeap::allocFromHead(size)",
            "diagnostic (SB_LOG=anmdata): the default-alignment overload — four entry points, not two")
SB_OVERRIDE(0x802d4cf0u, ov_deep_fn, "J3DSkinDeform-region recursive function",
            "diagnostic (SB_LOG=anmdata): count real recursion depth and guest stack usage — the "
            "watchpoint's 54 repeated frames were read off an already-corrupt stack")
SB_OVERRIDE(0x802c1a34u, ov_alloc_tail1, "JKRExpHeap::allocFromTail(size)",
            "diagnostic (SB_LOG=anmdata): the default-alignment tail overload")
SB_OVERRIDE(0x802c14d0u, ov_alloc_head, "JKRExpHeap::allocFromHead",
            "diagnostic (SB_LOG=anmdata): the path JKRExpHeap::alloc delegates to — where the memory "
            "is actually taken")
SB_OVERRIDE(0x802c18dcu, ov_alloc_tail, "JKRExpHeap::allocFromTail",
            "diagnostic (SB_LOG=anmdata): the tail-allocation path, same reason")
SB_OVERRIDE(0x802c52b0u, ov_jkr_thread_ctor, "JKRThread::JKRThread",
            "diagnostic (SB_LOG=anmdata): every JKRThread takes a 16 KB stack from the 128 KB system "
            "heap — count and caller")
SB_OVERRIDE(0x802c1b88u, ov_exp_free_all, "JKRExpHeap::freeAll",
            "diagnostic (SB_LOG=anmdata): clear the resident ledger on a wholesale release")
SB_OVERRIDE(0x802c138cu, ov_exp_alloc, "JKRExpHeap::alloc",
            "diagnostic (SB_LOG=anmdata): remember every system-heap allocation so the RESIDENT set "
            "can be reported when the heap runs out")
SB_OVERRIDE(0x802c1b14u, ov_exp_free, "JKRExpHeap::free",
            "diagnostic (SB_LOG=anmdata): the virtual free a delete actually reaches")
SB_OVERRIDE(0x802c37b8u, ov_heap_free, "JKRHeap::free",
            "diagnostic (SB_LOG=anmdata): the other half of the system-heap balance")
SB_OVERRIDE(0x802c3c38u, ov_op_new_heap, "operator new(size, JKRHeap*, int)",
            "diagnostic (SB_LOG=anmdata): the allocator getFirstFile actually uses")
SB_OVERRIDE(0x802bf41cu, ov_get_first_file, "JKRArchive::getFirstFile",
            "diagnostic (SB_LOG=anmdata): trace the archive-side half of a failed lookup")
SB_OVERRIDE(0x802bcb14u, ov_find_directory, "JKRArchive::findDirectory",
            "diagnostic (SB_LOG=anmdata): the innermost step — which path, from which directory id")
SB_OVERRIDE(0x802c3740u, ov_heap_alloc, "JKRHeap::alloc",
            "diagnostic (SB_LOG=anmdata): report allocations that return NULL — the game does not "
            "check, so a full heap surfaces as an unrelated null pointer somewhere else")
SB_OVERRIDE(0x802c3124u, ov_find_volume, "JKRFileLoader::findVolume",
            "diagnostic (SB_LOG=anmdata): name the volume a path resolves to — two mounted loaders "
            "can answer to the same first component")
SB_OVERRIDE(0x802bc9acu, ov_archive_ctor_default, "JKRArchive::JKRArchive()",
            "diagnostic (SB_LOG=anmdata): the other base constructor — hooking only one of the two "
            "makes the archive listing silently incomplete")
SB_OVERRIDE(0x802bc9f8u, ov_archive_ctor, "JKRArchive::JKRArchive(entrynum, mode)",
            "diagnostic (SB_LOG=anmdata): remember every mounted archive so a failed directory "
            "lookup can print what the archives actually contain")
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
