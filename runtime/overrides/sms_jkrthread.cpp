// PC-native replication of SMS's JKRThread async asset workers.
//
// The GameCube build runs asset decompression/streaming on JKRThread worker threads: the caller
// builds a command, OSSendMessage's it to the worker's queue, and OSReceiveMessage-waits for the
// reply; the worker's run() loop does the work. That's GC-specific concurrency infrastructure —
// and running the GC scheduler/threads in the recomp hybrid is exactly what we should NOT emulate
// (see memory port-not-emulate). On a PC port we don't need the threads: do the work SYNCHRONOUSLY
// on the calling thread. The game already ships the synchronous routine the worker calls
// (JKRDecomp::decode), so the replication is a direct call.

#include "../overrides.h"

extern "C" void func_802ecbd0(CPUState& cpu);   // JKRDecomp::decode(this, src, dst, srcLen, dstLen)

// JKRDecomp::orderSync (0x802ecb28): replace the OSSendMessage→worker→OSReceiveMessage round-trip
// with a direct call to decode() — same args (r3..r7), same result, no GC thread.
SUNBRIGHT_OVERRIDE(ov_JKRDecomp_orderSync, 0x802ecb28u) {
    func_802ecbd0(cpu);
}

// JKRDecomp::run (0x802eca38): the worker thread's command loop. With orderSync now synchronous, no
// commands are ever sent to it, so the worker has nothing to do — return immediately so the thread
// just exits instead of OSReceiveMessage-blocking forever (which would stall the GC scheduler).
SUNBRIGHT_OVERRIDE(ov_JKRDecomp_run, 0x802eca38u) {
    (void)cpu;   // return to caller; the thread function exits
}
