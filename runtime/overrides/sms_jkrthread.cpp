// SMS JKRThread async asset workers.
//
// The GameCube build runs asset decompression/streaming on JKRThread worker threads: the caller
// builds a command, OSSendMessage's it to the worker's queue, and OSReceiveMessage-waits for the
// reply; the worker's run() loop does the work.
//
// Now that native threading (nthr) OWNS the GC threads, those workers run for real on their own host
// threads, and the message handshake flows through the native OSSleepThread/OSWakeupThread — so we
// run the REAL async path by default (defer to the recompiled bodies). The old synchronous
// replication (do the decode directly on the caller, no worker) is kept behind SUNBRIGHT_JKR_SYNC=1
// for A/B / fallback — it was a workaround for not having working threads.
#include "../overrides.h"
#include <cstdlib>

extern "C" void func_802ecbd0(CPUState& cpu);   // JKRDecomp::decode(this, src, dst, srcLen, dstLen)

static bool jkr_sync_mode() {
    static const bool on = getenv("SUNBRIGHT_JKR_SYNC") != nullptr;
    return on;
}

// JKRDecomp::orderSync (0x802ecb28): default → the real async order (message round-trip to the
// native worker thread). SUNBRIGHT_JKR_SYNC → direct synchronous decode() on the caller.
SUNBRIGHT_OVERRIDE(ov_JKRDecomp_orderSync, 0x802ecb28u) {
    if (jkr_sync_mode()) { func_802ecbd0(cpu); return; }
    if (RecompFunc raw = recomp_raw(0x802ecb28u)) raw(cpu);
}

// JKRDecomp::run (0x802eca38): default → the real worker command loop (the native worker thread runs
// it, OSReceiveMessage-parks for work). SUNBRIGHT_JKR_SYNC → no-op (no commands are sent in sync mode,
// so the worker would block forever; just exit).
SUNBRIGHT_OVERRIDE(ov_JKRDecomp_run, 0x802eca38u) {
    if (jkr_sync_mode()) { (void)cpu; return; }
    if (RecompFunc raw = recomp_raw(0x802eca38u)) raw(cpu);
}
