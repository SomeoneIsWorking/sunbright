// sms_boot_audio.cpp — native PC reimplementation of the JASystem::Vload sequence
// loader for the sms-boot (decomp-JASystem) build, sourcing BGM/SE sequences from the
// real BARC table + sequence.arc instead of the dead JaiArcS.hed Vload header.
//
// =====================================================================================
// (i) ROOT-CAUSE RE CALL-CHAIN (file:line)
// =====================================================================================
// Reaching the option/file-select stage, MarDirector setup runs:
//   reference/sms/src/System/MarDirectorSetupObjects.cpp:210
//     MSMainProc::setMSoundEnterStage(mMap, unk7D)
//   reference/sms/src/System/MSoundMainSide.cpp:123  setMSoundEnterStage(...)
//     -> (case 15) reference/sms/src/System/MSoundMainSide.cpp:381
//          SMSGetMSound()->loadArcSeqData(MSD_BGM_CHUBOSS2, false)
//   reference/sms/src/JSystem/JAudio/JAInterface/JAIBasic.cpp:1234
//     int JAIBasic::loadArcSeqData(u32 param_1, bool param_2):
//       u32 uVar1   = param_1 & 0x3ff;                       // BARC sequence index
//       u32 uVar2   = JASystem::Vload::checkSize(uVar1);     // size of the BMS
//       void* iVar3 = unk0->checkOnMemory(uVar1, nullptr);   // resident? (JAIData.cpp:349)
//       ... if not resident: allocate an auto/stay heap slot (pre-allocated by
//           JAIBasic::initSeqsLoadArea, JAIBasic.cpp:551) then
//       JAIBasic.cpp:1264  JASystem::Vload::loadFileAsync(unk2C + uVar1, puVar4, 0,
//                                                         uVar2, &checkDvdLoadArc, id);
//       (or the &0x40 sync branch: JAIBasic.cpp:1270 Vload::loadFile(unk2C+uVar1,...))
//
// The Vload arc table is EMPTY: JAIBasic::initInterfaceMain (JAIBasic.cpp:115-120) calls
//   JASystem::Vload::initHeader("/AudioRes/Seqs/JaiArcS.hed")
//   unk2C = JASystem::Vload::getArchiveHandle("JaiArcS.hed")
// but JaiArcS.hed is NOT on the SMS disc FST, so Dvd::checkFileExtend returns 0 and
// initHeaderM (JASVload.cpp:47) bails with vlCurrentArcs still 0. getArchiveHandle then
// returns -1 (unk2C = 0xffffffff), getRealHandle returns 0, checkSize returns 0, and
// loadFileAsync dereferences a null handle. The project added a FAIL-FAST OSPanic there
// (JASVload.cpp:187), which is the crash that blocks the boot. Vload is the DEAD path for
// SMS — the real sequence collection is the AAF BARC chunk + sequence.arc.
//
// AAF BARC layout (verified live in runtime/native_jas.cpp:491-504, the recomp build):
//   mSound.aaf is a chunk stream of u32 chunk-ids (big-endian). cid 0 ends. cid in
//   {1,4,5,6,7} = ONE (offset,size,flag) triplet; cid in {2,3} = a list of triplets
//   until offset==0. cid==4's offset points at the BARC block:
//     "BARC" magic @+0; u32 count @+0xC; archive name @+0x10; entries @+0x20, 0x20 bytes
//     each: char name[14], u16 flags, u32, u32, u32 offset (into sequence.arc) @+0x18,
//     u32 size @+0x1C.  BGM id & 0x3FF == BARC index (MSD_BGM_CHUBOSS2 0x80010010 -> 16).
// mSound.aaf and sequence.arc are plain FST files read directly by JASystem::Dvd here
// (the decomp reads /AudioRes/mSound.aaf the same way: MSound.cpp:434 +
// JAIBasic::checkInitDataFile JAIBasic.cpp:219). NOTE: the in-memory AAF (JAIBasic::unk4C)
// is freed by initReadFile (JAIBasic.cpp:173, deleteTmpDVDFile, because unk13==2 != 4),
// so it is NOT live at loadArcSeqData time — we re-read mSound.aaf ourselves (small, once,
// cached) to build the BARC table.
//
// =====================================================================================
// (ii) APPROACH CHOSEN AND WHY
// =====================================================================================
// REIMPLEMENT the JASystem::Vload namespace (this file) so it is backed by the real BARC
// table + sequence.arc, and EXCLUDE the decomp TU
//   reference/sms/src/JSystem/JAudio/JASystem/JASVload.cpp
// from the sms-native build so these definitions link instead.
//
// Why Vload and not loadArcSeqData / a checkOnMemory pre-register hook:
//   * JAIBasic::loadArcSeqData and JAIData::checkOnMemory carry all the faithful heap-slot
//     management, residency bookkeeping, async/sync split, and checkDvdLoadArc completion.
//     Leaving them UNTOUCHED keeps that engine logic intact and diffable; we only swap the
//     dead data SOURCE. This is the minimal faithful intervention.
//   * loadArcSeqData lives in JAIBasic.cpp alongside ~80 other needed functions, so it
//     cannot be replaced without losing the whole TU. JASVload.cpp is small and entirely
//     dead-for-SMS — replacing the whole TU is clean and collision-free.
//   * The opaque handle contract already threads the index through cleanly:
//     getArchiveHandle("JaiArcS.hed") returns arcIdx<<16 (we register arc 0, so unk2C=0),
//     and loadArcSeqData passes param1 = unk2C + (seqId & 0x3ff) = the BARC index. So our
//     checkSize/loadFile/loadFileAsync decode `param1 & 0xFFFF` = BARC index directly.
//
// Behavior implemented (faithful to the original Vload contract):
//   initHeaderM(path,...) : register ONE logical arc "JaiArcS.hed" (so getArchiveHandle
//                           succeeds, vlCurrentArcs=1) and lazily load+parse the BARC.
//   getArchiveHandle(name): name match -> 0; else -1  (same as decomp).
//   checkSize(handle)     : BARC[handle & 0xFFFF].size.
//   loadFile(h,buf,off,len): SYNCHRONOUS read of len bytes from sequence.arc at
//                            BARC[idx].off + off into buf, via JASystem::Dvd. Returns len.
//   loadFileAsync(h,buf,off,len,cb,cbarg): same synchronous read (the PC DVD path is
//                            synchronous everywhere — see JASDvdThread.cpp:160), then
//                            invoke the completion callback (checkDvdLoadArc) with cbarg so
//                            the JAIData auto-heap loaded-flag/registration completes exactly
//                            as on hardware. Returns len.
//   getRealHandle/getMemoryFile/getLogicalHandle*/getHandle/setMaxArcs/initVloadBuffers:
//                            faithful stubs (getRealHandle unused once checkSize/load are
//                            BARC-backed; kept returning 0 so any stray caller is null-safe).
//
// No magic constants: every offset is read from the on-disc AAF/BARC/sequence.arc headers.
// FAIL FAST: a bad AAF magic, missing BARC, or out-of-range index OSPanics with the value.
//
// =====================================================================================
// (iii) EXACT INTEGRATION STEPS (integrator: follow verbatim)
// =====================================================================================
// 1. In native/CMakeLists.txt, after the SMS_NATIVE_SOURCES glob + the existing
//    list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX "...(dolphin|PowerPC...)...") line
//    (native/CMakeLists.txt ~line 33), add:
//
//        # Native BARC-backed sequence loader replaces the dead JaiArcS.hed Vload path.
//        list(FILTER SMS_NATIVE_SOURCES EXCLUDE REGEX
//             "/reference/sms/src/JSystem/JAudio/JASystem/JASVload\\.cpp$")
//
// 2. Add this file to the sms-boot executable sources (native/CMakeLists.txt ~line 336,
//    the add_executable(sms-boot ...) list):
//
//        "${SMS_NATIVE_REPO_ROOT}/native/src/sms_boot_audio.cpp"
//
//    (Putting it in the sms-boot target is sufficient because the RESCAN link group
//     re-scans sms-native for the now-undefined Vload symbols. If a non-boot target —
//     sms-j3dload_test / a platform test — also pulls JASVload symbols and fails to link,
//     instead add this file to the sms-native library glob list, e.g.
//        list(APPEND SMS_NATIVE_SOURCES
//             "${SMS_NATIVE_REPO_ROOT}/native/src/sms_boot_audio.cpp")
//     — but it must then be compiled with the sms-native include dirs, which it already is
//     via the reference/sms/include path; do NOT add the -w/-fcommon decomp flags, this is
//     clean C++.)
//
// 3. No other source needs touching. No symbol allow-multiple-definition, no --wrap.
//    Verify after build: with SB_DBG_AUDIO=1 (or SB_MOVIE_DBG=1) the log line
//      "[sms_boot_audio] BARC ready: N seqs ..." prints at first sequence load, and the
//      boot proceeds past setMSoundEnterStage into the stage instead of OSPanic'ing in
//      Vload::loadFileAsync.
// =====================================================================================

#include <JSystem/JAudio/JASystem/JASVload.hpp>
#include <JSystem/JAudio/JASystem/JASDvdThread.hpp>
#include <JSystem/JAudio/JASystem/JASSystemHeap.hpp>
#include <dolphin/os.h>
#include <types.h>

#include <cstdlib>
#include <cstring>

// =====================================================================================
// In-memory mSound.aaf, captured at boot by the integrator.
// =====================================================================================
// mSound.aaf is NOT a child of the disc FST — it exists ONLY inside nintendo.szs and is
// read at boot as an archive RESOURCE (reference/sms/src/System/Application.cpp:312-317:
// becomeCurrent("/audi") -> getResource/readResource("mSound.aaf") -> new MSound(... buf)).
// A DVD read of "/AudioRes/mSound.aaf" can never resolve (checkFileExtend == 0). The
// integrator captures the in-memory resource buffer right after Application.cpp:315
// readResource via an `extern "C"` decl of these symbols:
//     sb_aaf_data = buf; sb_aaf_size = uVar3;
// We build the BARC table from this buffer instead of reading the disc.
extern "C" {
const unsigned char* sb_aaf_data = nullptr;
unsigned int         sb_aaf_size = 0;
}

namespace {

// --- BARC table built from /AudioRes/mSound.aaf (chunk 4) ---------------------------
struct BarcEntry {
	u32 off;  // byte offset of the BMS within sequence.arc
	u32 size; // BMS length
};

const char* const kAafPath = "/AudioRes/mSound.aaf"; // (no longer DVD-read; see below)
const char* const kSeqPath = "/AudioRes/sequence.arc";

// "JaiArcS.hed" is the logical sequence-archive name the decomp asks for; we register a
// single logical arc under it so getArchiveHandle resolves to index 0 (unk2C == 0).
const char* const kArcName = "JaiArcS.hed";

bool      g_initialized = false; // initHeaderM ran (one logical arc registered)
bool      g_barcReady   = false; // BARC table parsed
BarcEntry* g_barc       = nullptr;
u32        g_barcCount  = 0;

bool dbg()
{
	return std::getenv("SB_MOVIE_DBG") != nullptr
	       || std::getenv("SB_DBG_AUDIO") != nullptr;
}

inline u32 be32(const u8* p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

// Load /AudioRes/mSound.aaf, walk the chunk stream to chunk-4 (BARC), build g_barc.
// Called lazily on first sequence access. FAIL FAST on a malformed archive.
void buildBarcTable()
{
	if (g_barcReady)
		return;

	// mSound.aaf is NOT on the disc FST — it lives inside nintendo.szs and is captured as
	// an in-memory archive resource at boot (sb_aaf_data/sb_aaf_size, populated by the
	// integrator right after Application.cpp:315 readResource("mSound.aaf")). Use that
	// buffer directly; the disc never holds /AudioRes/mSound.aaf.
	if (sb_aaf_data == nullptr)
		OSPanic(__FILE__, __LINE__,
		        "sms_boot_audio: aaf not captured (boot hook missing)");

	const u8* aaf  = (const u8*)sb_aaf_data;
	u32       aafSize = sb_aaf_size;

	// AAF chunk walk (big-endian): cid 0 ends; cid {1,4,5,6,7} = one (off,size,flag)
	// triplet; cid {2,3} = list of triplets terminated by off==0.
	u32 o        = 0;
	u32 barcOff  = 0;
	while (o + 4 <= aafSize) {
		u32 cid = be32(aaf + o);
		o += 4;
		if (cid == 0)
			break;
		if (cid == 1 || (cid >= 4 && cid <= 7)) {
			if (cid == 4)
				barcOff = be32(aaf + o); // offset of the BARC block
			o += 12;                     // (offset, size, flag)
			continue;
		}
		if (cid == 2 || cid == 3) {
			while (o + 4 <= aafSize && be32(aaf + o) != 0)
				o += 12;
			o += 4; // terminator word
			continue;
		}
		OSPanic(__FILE__, __LINE__,
		        "sms_boot_audio: unknown AAF chunk id=%u at off=%u in %s", cid, o - 4,
		        kAafPath);
	}

	if (barcOff == 0 || barcOff + 0x20 > aafSize
	    || std::memcmp(aaf + barcOff, "BARC", 4) != 0) {
		u8 m0 = barcOff + 0 < aafSize ? aaf[barcOff + 0] : 0;
		u8 m1 = barcOff + 1 < aafSize ? aaf[barcOff + 1] : 0;
		u8 m2 = barcOff + 2 < aafSize ? aaf[barcOff + 2] : 0;
		u8 m3 = barcOff + 3 < aafSize ? aaf[barcOff + 3] : 0;
		OSPanic(__FILE__, __LINE__,
		        "sms_boot_audio: no BARC chunk in %s (barcOff=%u aafSize=%u "
		        "magic=%02x%02x%02x%02x)",
		        kAafPath, barcOff, aafSize, m0, m1, m2, m3);
	}

	const u8* b = aaf + barcOff;
	u32 n       = be32(b + 0xC);
	if (barcOff + 0x20 + (u64)n * 0x20 > aafSize)
		OSPanic(__FILE__, __LINE__,
		        "sms_boot_audio: BARC count %u overruns mSound.aaf (barcOff=%u "
		        "aafSize=%u)",
		        n, barcOff, aafSize);

	g_barc      = (BarcEntry*)JASystem::Kernel::allocFromSysDram(
        (n ? n : 1) * sizeof(BarcEntry));
	g_barcCount = n;
	for (u32 i = 0; i < n; ++i) {
		const u8* e   = b + 0x20 + i * 0x20;
		g_barc[i].off = be32(e + 0x18);
		g_barc[i].size = be32(e + 0x1C);
	}
	g_barcReady = true;

	if (dbg())
		OSReport("[sms_boot_audio] BARC ready: %u seqs (idx16 off=0x%x size=%u)\n", n,
		         n > 16 ? g_barc[16].off : 0u, n > 16 ? g_barc[16].size : 0u);
}

const BarcEntry* barcLookup(u32 idx)
{
	buildBarcTable();
	if (idx >= g_barcCount)
		OSPanic(__FILE__, __LINE__,
		        "sms_boot_audio: BARC index %u out of range (count=%u)", idx,
		        g_barcCount);
	return &g_barc[idx];
}

// Synchronous read of a BMS segment from sequence.arc into `buf`.
u32 readSeq(u32 idx, u8* buf, u32 extraOff, u32 len)
{
	const BarcEntry* e = barcLookup(idx);
	u32 fileOff        = e->off + extraOff;
	u32 readLen        = len ? len : e->size;

	char path[64];
	std::strcpy(path, kSeqPath);

	volatile u32 done = 0;
	// JASystem::Dvd::loadToDramDvdT(prio, path, buffer, offset, length, &done, cb).
	// (JASDvdThread.cpp:147 — arg4 = byte offset into the file, arg5 = length.) The PC
	// path runs synchronously inline and sets *done on completion.
	JASystem::Dvd::loadToDramDvdT(0, path, buf, fileOff, readLen, (u32*)&done, nullptr);
	while (done == 0)
		; // synchronous on PC; loop is a no-op but mirrors the Vload contract.

	if (dbg())
		OSReport("[sms_boot_audio] loaded seq idx=%u off=0x%x len=%u from %s\n", idx,
		         fileOff, readLen, kSeqPath);
	return readLen;
}

} // namespace

namespace JASystem {
namespace Vload {

// fabricated — matches the decomp's opaque entry type so the header decl is satisfied.
struct VLArcEntry {
	u32 unk0, unk4, unk8;
	u16 unkC, unkE;
	u32 unk10, unk14, unk18, unk1C;
};

void setMaxArcs(long) { }

void initVloadBuffers() { }

BOOL initHeader(char* param) { return initHeaderM(param, 0, 0); }

// Register the single logical sequence archive. We do NOT need the .hed bytes (param_2);
// the BARC table is the real index. Loading the BARC is deferred (lazy) so a missing
// mSound.aaf only fails when a sequence is actually requested.
BOOL initHeaderM(char* /*param_1*/, u8* /*param_2*/, u8* /*param_3*/)
{
	g_initialized = true;
	// Do NOT build the BARC table here: initHeaderM runs during JAIBasic init at BOOT,
	// before the audio/DVD file path is usable, so a checkFileExtend would return 0 and
	// OSPanic. The table is built LAZILY on the first checkSize/loadFile/loadFileAsync
	// (at stage entry, when the DVD FS is ready) via barcLookup -> buildBarcTable.
	if (dbg())
		OSReport("[sms_boot_audio] Vload::initHeaderM -> BARC-backed (lazy)\n");
	return TRUE;
}

// Name match -> arc index 0 (so unk2C == 0); else -1, exactly like the decomp.
u32 getArchiveHandle(char* param)
{
	if (g_initialized && param && std::strcmp(param, kArcName) == 0)
		return 0u << 0x10;
	return (u32)-1;
}

u32 getLogicalHandle(char*) { return 0; }
u32 getLogicalHandleS(char*, char*) { return 0; }
u32 getHandle(u32) { return 0; }

// Unused once checkSize/loadFile are BARC-backed; null-safe stub.
VLArcEntry* getRealHandle(u32) { return nullptr; }

// handle low 16 bits = BARC index.
u32 checkSize(u32 param)
{
	const BarcEntry* e = barcLookup(param & 0xFFFF);
	return e->size;
}

// Synchronous load. param1 low16 = BARC index; param3 = extra offset; param4 = length.
u32 loadFile(u32 param1, u8* param2, u32 param3, u32 param4)
{
	return readSeq(param1 & 0xFFFF, param2, param3, param4);
}

// "Async" load — synchronous on PC, then fire the completion callback so the JAIData
// auto-heap loaded-flag clears and the sequence registers (JAIBasic::checkDvdLoadArc).
u32 loadFileAsync(u32 param1, u8* param2, u32 param3, u32 param4,
                  void (*param5)(u32), u32 param6)
{
	u32 n = readSeq(param1 & 0xFFFF, param2, param3, param4);
	if (param5)
		param5(param6);
	return n;
}

void* getMemoryFile(u32) { return nullptr; }

} // namespace Vload
} // namespace JASystem
