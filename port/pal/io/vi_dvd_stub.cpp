// ===========================================================================
// port/pal/io/vi_dvd_stub.cpp — portable PAL implementations of the GameCube
// VI (video-interface) retrace counter and the DVD path->entrynum lookup that
// the SMS engine references but that have no native hardware yet.
//
// WHY THIS EXISTS
// ---------------
// A link-surface scan of libsmsport_core.a leaves VIGetRetraceCount and
// DVDConvertPathToEntrynum UNDEFINED. Both are GameCube SDK wrappers over
// hardware/firmware (the VI retrace counter and the on-disc DVD filesystem)
// that has no PC counterpart. They are declared `extern "C"` by the real SDK
// headers (<dolphin/vi/vifuncs.h>, <dolphin/dvd.h>); including those and
// defining against them matches the signatures verbatim.
//
// ---------------------------------------------------------------------------
// VIGetRetraceCount — u32 VIGetRetraceCount(void)
//
//   On hardware this is a monotonically increasing count of video fields
//   (~59.94/s NTSC). It MUST advance over wall time: several engine callers
//   busy-wait on it and would otherwise spin forever, e.g.
//     JDRVideo.cpp:    while (mNextRetraceIndex - (int)VIGetRetraceCount() > 1)
//     JUTException.cpp:while (start == VIGetRetraceCount())
//   A constant return (0) would hang those loops. We therefore derive the count
//   from the host monotonic clock at the NTSC field rate (~59.94 Hz). This is
//   the right layer for bring-up: the engine treats it as a wall-clock field
//   tick, not a render-synced VBlank. (When a native VI/present loop lands,
//   replace this with the real per-field increment so it is frame-accurate.)
//
//   FLAG FOR PM: this is a bring-up placeholder. It advances by WALL TIME, not
//   by actual presented video fields — so retrace-paced timing is only as
//   accurate as the host clock, and any code that assumes 1 increment == 1
//   rendered frame will drift from the real renderer until native VI lands.
//
// VIGetNextField — u32 VIGetNextField(void)
//
//   Returns which field (0 = even/bottom, 1 = odd/top) the next retrace will
//   display, for interlaced field pairing. Callers (THPPlayer.c FMV pacing)
//   compare it to 0/1 to alternate frames. We alternate it in lockstep with the
//   retrace count (parity of the field index), so the two stay consistent and a
//   THP field-wait progresses instead of stalling. Declared in the header even
//   though not yet on the undefined list — defining it keeps the VI field pair
//   complete and is harmless.
//
// ---------------------------------------------------------------------------
// DVDConvertPathToEntrynum — s32 DVDConvertPathToEntrynum(char* pathPtr)
//
//   On hardware this walks the on-disc DVD filesystem (FST) and returns the
//   file/dir entry index for an absolute path, or -1 if not present. The native
//   port does NOT have a DVD FS: the real asset path is the endian-safe RARC
//   reader in port/assets (rarc.{h,cpp}), which is a separate concern.
//
//   BRING-UP PLACEHOLDER: return -1 ("not found") for every path. Every caller
//   (JKRArchivePub, Application, JAISystemInterface, JASDvdThread) already
//   handles the -1 case as "file absent" and falls back / skips, so returning
//   -1 is the safe, faithful "no DVD filesystem mounted" answer. Replace this
//   with a real FST/host file-I/O lookup when native DVD/file I/O lands.
//
//   FLAG FOR PM: this is a placeholder that reports EVERY DVD path as missing.
//   Any engine path that loads assets via the DVD FS (rather than the native
//   RARC reader) will see "file not found" until native DVD/file I/O is built.
// ===========================================================================

#include <dolphin/dvd.h>
#include <dolphin/vi/vifuncs.h>

#include <chrono>
#include <cstdint>

namespace {

// Host-clock-derived NTSC field counter. ~59.94 fields/s; integer division of
// elapsed nanoseconds by the per-field period gives a monotonic count that
// advances over wall time so retrace busy-waits make progress.
//
// Period: 1/59.94 s ~= 16.683 ms ~= 16683333 ns per field.
constexpr std::int64_t kFieldPeriodNs = 16683333;

std::uint32_t HostFieldCount() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    const std::int64_t elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start)
            .count();
    return static_cast<std::uint32_t>(elapsed_ns / kFieldPeriodNs);
}

}  // namespace

extern "C" {

u32 VIGetRetraceCount(void) {
    return HostFieldCount();
}

u32 VIGetNextField(void) {
    // Field parity stays in lockstep with the retrace count so a THP field-wait
    // (compares to 0/1) alternates instead of stalling.
    return HostFieldCount() & 1u;
}

s32 DVDConvertPathToEntrynum(char* /*pathPtr*/) {
    // No native DVD filesystem during bring-up: report every path as not found.
    // Callers handle -1 as "file absent". See header comment (PM placeholder).
    return -1;
}

}  // extern "C"
