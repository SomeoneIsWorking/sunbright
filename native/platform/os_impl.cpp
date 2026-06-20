// os_impl.cpp — native PC implementation of the GameCube OS kernel seam (E1).
//
// Implements the 61 unresolved OS* symbols the game logic references (grounded in
// scratch/native_unresolved.txt). The game embeds the SDK structs (OSThread,
// OSMutex, OSMessageQueue, OSCond, OSStopwatch) directly in its objects, so we
// KEEP their SDK layouts and carry native backing state (std::thread, condvars)
// in a side-table keyed by the SDK-struct pointer.
//
// THREADING MODEL (option A from native/platform/README.md / os_seam.h):
//   - Real host std::thread per OSThread (created suspended; OSResumeThread starts).
//   - Mutex (recursive — GC OSMutex IS recursive, carries a count), Cond, and the
//     bounded message queue use std::mutex/condition_variable for blocking with
//     correct FIFO / signal semantics.
//   - Interrupts (OSDisableInterrupts/Restore/Enable) map to a recursive GLOBAL
//     lock so a "disabled-interrupts" critical section is mutually exclusive with
//     every other game thread — standing in for the GC's single-core atomicity.
//
// VERIFIED: the primitive CONTRACTS are unit-tested in tests/os_test.cpp (mutual
// exclusion, recursive relock, FIFO message passing + blocking, cond wait/signal,
// thread create/resume/join/exit, time monotonicity, heap alloc/free/referent-size).
// NOT YET VERIFIED (deferred to boot, honest gap): GC fixed-PRIORITY scheduling
// order and true cooperative non-preemption — host threads are preempted by the
// kernel scheduler, not picked by OS priority. The global interrupt-lock covers the
// critical-section races; if a priority-ordering dependency surfaces at boot,
// escalate to the cooperative-fiber backend (option B). LANDMINEs flagged inline.

#include <dolphin/os.h>
#include <dolphin/os/OSThread.h>
#include <dolphin/os/OSMutex.h>
#include <dolphin/os/OSMessage.h>
#include <dolphin/os/OSStopwatch.h>
#include <dolphin/os/OSAlloc.h>
#include <dolphin/os/OSCache.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {

// GC time base = bus clock / 4 = 162 MHz / 4 = 40.5 MHz.
constexpr long long kTBClock = 40500000LL;

using clock_t_ = std::chrono::steady_clock;
clock_t_::time_point g_boot = clock_t_::now();

long long now_ticks() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  clock_t_::now() - g_boot).count();
    // ticks = ns * tbclock / 1e9, done in 128-bit-safe order.
    return (long long)((__int128)ns * kTBClock / 1000000000LL);
}

// ---- thread backing -------------------------------------------------------
struct NativeThread {
    std::thread th;
    void* (*func)(void*) = nullptr;
    void* param = nullptr;
    OSThread* os = nullptr;
    std::mutex m;
    std::condition_variable cv;       // gates start (resume) and join-wakeups
    bool resumed = false;
    bool finished = false;
    bool detached = false;
    bool cancel = false;
    void* retval = nullptr;
};

// Internal sentinel thrown by OSExitThread / OSCancelThread to unwind the thread
// body (GC OSExitThread is noreturn; we unwind to the wrapper).
struct ThreadExit { void* val; };

std::mutex g_table_mu;
std::unordered_map<OSThread*, NativeThread*> g_threads;
thread_local OSThread* t_current = nullptr;

NativeThread* backing(OSThread* t) {
    std::lock_guard<std::mutex> lk(g_table_mu);
    auto it = g_threads.find(t);
    return it == g_threads.end() ? nullptr : it->second;
}

// ---- global interrupt lock (critical-section mutual exclusion) ------------
std::recursive_mutex g_intr;
thread_local int t_intr_depth = 0;

// ---- heap / arena ---------------------------------------------------------
std::mutex g_heap_mu;
std::unordered_map<void*, unsigned long> g_referent;  // ptr -> size (OSReferentSize)
void* g_arena_lo = nullptr;
void* g_arena_hi = nullptr;
std::atomic<int> g_next_heap{0};

// ---- misc HW-vestigial state ---------------------------------------------
std::atomic<unsigned int> g_sound_mode{OS_SOUND_MODE_STEREO};
std::atomic<unsigned int> g_progressive_mode{0};
std::atomic<bool> g_reset_requested{false};

} // namespace

extern "C" {

// ===========================================================================
// Report / Panic
// ===========================================================================
void OSReport(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    std::fprintf(stderr, "OSPanic: %s:%d: ", file ? file : "?", line);
    va_list ap; va_start(ap, msg);
    std::vfprintf(stderr, msg, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    std::abort();
}

// ===========================================================================
// Time
// ===========================================================================
OSTime OSGetTime(void)  { return (OSTime)now_ticks(); }
OSTick OSGetTick(void)  { return (OSTick)(now_ticks() & 0xFFFFFFFF); }

// Calendar conversion. GC epoch is 2000-01-01 00:00:00; ticks count from boot, but
// for SMS the value is only used for relative save timestamps / display, so we
// treat the tick count as seconds-since-an-epoch and break it down with a faithful
// civil-from-days algorithm (Howard Hinnant's), anchored at year 2000.
void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
    long long t = (long long)ticks;
    long long secsTotal = t / kTBClock;
    long long rem = t % kTBClock;
    if (rem < 0) { rem += kTBClock; secsTotal -= 1; }
    td->usec = (int)((rem * 1000000LL) / kTBClock % 1000);
    td->msec = (int)((rem * 1000LL) / kTBClock);

    long long days = secsTotal / 86400;
    long long sod  = secsTotal % 86400;
    if (sod < 0) { sod += 86400; days -= 1; }
    td->hour = (int)(sod / 3600);
    td->min  = (int)((sod % 3600) / 60);
    td->sec  = (int)(sod % 60);

    // days since 2000-01-01 -> civil date. days_from_civil epoch shift: 2000-01-01
    // is day 10957 in the 1970 epoch; we anchor at 2000 directly.
    long long z = days + 730425;  // shift to internal era epoch (0000-03-01 based)
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2)/153;
    unsigned d = doy - (153*mp+2)/5 + 1;
    unsigned mo = mp < 10 ? mp+3 : mp-9;
    y += (mo <= 2);
    td->year = (int)y;
    td->mon  = (int)mo - 1;  // GC OSCalendarTime mon is 0-based
    td->mday = (int)d;
    // weekday: 1970-01-01 was Thursday(4); compute from total days since 1970.
    long long days1970 = days + 10957;  // 2000-01-01 is 10957 days after 1970-01-01
    td->wday = (int)(((days1970 % 7) + 4 + 7) % 7);
    td->yday = (int)doy;  // approximate (era day-of-year), good enough for display
}

// ===========================================================================
// Interrupts (global recursive critical-section lock)
// ===========================================================================
BOOL OSDisableInterrupts(void) {
    BOOL wasEnabled = (t_intr_depth == 0) ? TRUE : FALSE;
    g_intr.lock();
    ++t_intr_depth;
    return wasEnabled;
}
BOOL OSRestoreInterrupts(BOOL /*level*/) {
    // Paired 1:1 with a prior OSDisableInterrupts (the universal game idiom).
    BOOL wasEnabled = (t_intr_depth == 0) ? TRUE : FALSE;
    if (t_intr_depth > 0) { --t_intr_depth; g_intr.unlock(); }
    return wasEnabled;
}
BOOL OSEnableInterrupts(void) {
    BOOL wasEnabled = (t_intr_depth == 0) ? TRUE : FALSE;
    if (t_intr_depth > 0) { --t_intr_depth; g_intr.unlock(); }
    return wasEnabled;
}

// ===========================================================================
// Threads
// ===========================================================================
void OSInitThreadQueue(OSThreadQueue* q) { q->head = nullptr; q->tail = nullptr; }

OSThread* OSGetCurrentThread(void) { return t_current; }

BOOL OSIsThreadTerminated(OSThread* t) {
    NativeThread* n = backing(t);
    return (n && n->finished) ? TRUE : FALSE;
}

static void thread_trampoline(NativeThread* n) {
    t_current = n->os;
    {  // wait until resumed
        std::unique_lock<std::mutex> lk(n->m);
        n->cv.wait(lk, [n]{ return n->resumed; });
    }
    void* rv = nullptr;
    if (!n->cancel) {
        n->os->state = OS_THREAD_STATE_RUNNING;
        try {
            rv = n->func(n->param);
        } catch (ThreadExit& e) {
            rv = e.val;
        }
    }
    bool detached;
    {
        std::lock_guard<std::mutex> lk(n->m);
        n->retval = rv;
        n->finished = true;
        n->os->state = OS_THREAD_STATE_MORIBUND;
        n->os->val = rv;
        detached = n->detached;
        n->cv.notify_all();  // wake joiners
    }
    if (detached) {
        // No joiner will collect us; self-clean.
        std::thread::id myid = std::this_thread::get_id();
        (void)myid;
        std::lock_guard<std::mutex> tlk(g_table_mu);
        g_threads.erase(n->os);
        n->th.detach();
        delete n;
    }
}

int OSCreateThread(OSThread* thread, void* (*func)(void*), void* param,
                   void* stack, u32 stackSize, s32 priority, u16 attr) {
    (void)stack; (void)stackSize;
    NativeThread* n = new NativeThread();
    n->func = func; n->param = param; n->os = thread;
    n->detached = (attr & OS_THREAD_ATTR_DETACH) != 0;

    std::memset(thread, 0, sizeof(*thread));
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attr;
    thread->priority = priority;
    thread->base = priority;
    thread->suspend = 1;  // created suspended (GC contract)
    thread->val = (void*)-1;
    OSInitThreadQueue(&thread->queueJoin);

    {
        std::lock_guard<std::mutex> lk(g_table_mu);
        g_threads[thread] = n;
    }
    n->th = std::thread(thread_trampoline, n);
    return 1;
}

long OSResumeThread(OSThread* thread) {
    NativeThread* n = backing(thread);
    if (!n) return 0;
    long prev = thread->suspend;
    if (thread->suspend > 0) thread->suspend--;
    if (thread->suspend == 0) {
        std::lock_guard<std::mutex> lk(n->m);
        if (!n->resumed) { n->resumed = true; n->cv.notify_all(); }
        if (thread->state == OS_THREAD_STATE_READY)
            thread->state = OS_THREAD_STATE_RUNNING;
    }
    return prev;
}

void OSExitThread(OSThread* thread) {
    // GC OSExitThread is noreturn; the exit value is the thread's ->val. The decomp
    // calls OSExitThread(self). We unwind to the trampoline via throw (caught there).
    void* rv = thread ? thread->val : (t_current ? t_current->val : nullptr);
    throw ThreadExit{ rv };
}

int OSJoinThread(OSThread* thread, void* val) {
    NativeThread* n = backing(thread);
    if (!n) return 0;
    void* rv;
    {
        std::unique_lock<std::mutex> lk(n->m);
        n->cv.wait(lk, [n]{ return n->finished; });
        rv = n->retval;
    }
    if (val) *(void**)val = rv;
    // Reap.
    if (n->th.joinable()) n->th.join();
    {
        std::lock_guard<std::mutex> tlk(g_table_mu);
        g_threads.erase(thread);
    }
    delete n;
    return 1;
}

void OSDetachThread(OSThread* thread) {
    NativeThread* n = backing(thread);
    if (!n) return;
    thread->attr |= OS_THREAD_ATTR_DETACH;
    std::lock_guard<std::mutex> lk(n->m);
    n->detached = true;
}

void OSCancelThread(OSThread* thread) {
    // Best-effort cooperative cancel: if the thread hasn't started running yet,
    // flag it so its trampoline skips the body; a running thread cannot be safely
    // force-killed (no async unwind), so it runs to completion. LANDMINE: faithful
    // mid-flight cancellation needs the cooperative-fiber backend.
    NativeThread* n = backing(thread);
    if (!n) return;
    std::lock_guard<std::mutex> lk(n->m);
    n->cancel = true;
    thread->state = OS_THREAD_STATE_MORIBUND;
    if (!n->resumed) { n->resumed = true; n->cv.notify_all(); }
}

long OSGetThreadPriority(OSThread* thread) { return thread->priority; }

void OSYieldThread(void) { std::this_thread::yield(); }

s32 OSEnableScheduler(void) { return 0; }  // no explicit scheduler to gate

// ThreadQueue sleep/wake — used for ad-hoc wait queues. We back the queue with a
// process-global condvar keyed by the queue pointer (queues are caller-owned SDK
// structs; a side map gives them wait/notify without changing layout).
namespace { struct QWait { std::mutex m; std::condition_variable cv; int gen = 0; };
std::mutex g_qmu; std::unordered_map<OSThreadQueue*, QWait*> g_qwaits;
QWait* qwait(OSThreadQueue* q){ std::lock_guard<std::mutex> lk(g_qmu);
    auto& p = g_qwaits[q]; if(!p) p = new QWait(); return p; } }

void OSWakeupThread(OSThreadQueue* q) {
    QWait* w = qwait(q);
    std::lock_guard<std::mutex> lk(w->m);
    ++w->gen;
    w->cv.notify_all();
}

// ===========================================================================
// Mutex (recursive) / Cond
// ===========================================================================
void OSInitMutex(OSMutex* m) {
    m->queue.head = m->queue.tail = nullptr;
    m->thread = nullptr;
    m->count = 0;
    m->link.next = m->link.prev = nullptr;
}

// One global recursive mutex protects mutex bookkeeping AND provides the actual
// blocking. Each OSMutex carries its owner+count in the SDK fields. We use a single
// monitor lock + condvar to serialize; correct, if not maximally parallel.
namespace { std::mutex g_mtx_mon; std::condition_variable g_mtx_cv; }

void OSLockMutex(OSMutex* m) {
    OSThread* self = t_current;
    std::unique_lock<std::mutex> lk(g_mtx_mon);
    if (m->thread == self && m->count > 0) { m->count++; return; }   // recursive
    g_mtx_cv.wait(lk, [m]{ return m->thread == nullptr; });
    m->thread = self;
    m->count = 1;
}

BOOL OSTryLockMutex(OSMutex* m) {
    OSThread* self = t_current;
    std::lock_guard<std::mutex> lk(g_mtx_mon);
    if (m->thread == self && m->count > 0) { m->count++; return TRUE; }
    if (m->thread == nullptr) { m->thread = self; m->count = 1; return TRUE; }
    return FALSE;
}

void OSUnlockMutex(OSMutex* m) {
    std::lock_guard<std::mutex> lk(g_mtx_mon);
    if (m->count > 0 && --m->count == 0) {
        m->thread = nullptr;
        g_mtx_cv.notify_all();
    }
}

void OSInitCond(OSCond* c) { c->queue.head = c->queue.tail = nullptr; }

void OSWaitCond(OSCond* c, OSMutex* m) {
    // Release the mutex (fully — recursive count), wait, reacquire to same count.
    OSThread* self = t_current;
    int saved;
    {
        std::lock_guard<std::mutex> lk(g_mtx_mon);
        saved = m->count;
        m->count = 0; m->thread = nullptr;
        g_mtx_cv.notify_all();
    }
    {
        QWait* w = qwait(&c->queue);
        std::unique_lock<std::mutex> lk(w->m);
        int g = w->gen;
        w->cv.wait(lk, [w, g]{ return w->gen != g; });
    }
    {  // reacquire
        std::unique_lock<std::mutex> lk(g_mtx_mon);
        g_mtx_cv.wait(lk, [m]{ return m->thread == nullptr; });
        m->thread = self; m->count = saved;
    }
}

void OSSignalCond(OSCond* c) {
    QWait* w = qwait(&c->queue);
    std::lock_guard<std::mutex> lk(w->m);
    ++w->gen;
    w->cv.notify_all();
}

// ===========================================================================
// Message queue (bounded ring, FIFO, blocking + nonblocking + jam)
// ===========================================================================
namespace { std::mutex g_mq_mon; std::condition_variable g_mq_cv; }

void OSInitMessageQueue(OSMessageQueue* mq, void* msgArray, long msgCount) {
    mq->queueSend.head = mq->queueSend.tail = nullptr;
    mq->queueReceive.head = mq->queueReceive.tail = nullptr;
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
    mq->firstIndex = 0;
    mq->usedCount = 0;
}

int OSSendMessage(OSMessageQueue* mq, void* msg, long flags) {
    std::unique_lock<std::mutex> lk(g_mq_mon);
    if (mq->usedCount >= mq->msgCount) {
        if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
        g_mq_cv.wait(lk, [mq]{ return mq->usedCount < mq->msgCount; });
    }
    long idx = (mq->firstIndex + mq->usedCount) % mq->msgCount;
    ((OSMessage*)mq->msgArray)[idx] = msg;
    mq->usedCount++;
    g_mq_cv.notify_all();
    return TRUE;
}

int OSJamMessage(OSMessageQueue* mq, void* msg, long flags) {
    std::unique_lock<std::mutex> lk(g_mq_mon);
    if (mq->usedCount >= mq->msgCount) {
        if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
        g_mq_cv.wait(lk, [mq]{ return mq->usedCount < mq->msgCount; });
    }
    mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
    ((OSMessage*)mq->msgArray)[mq->firstIndex] = msg;
    mq->usedCount++;
    g_mq_cv.notify_all();
    return TRUE;
}

int OSReceiveMessage(OSMessageQueue* mq, void* msg, long flags) {
    std::unique_lock<std::mutex> lk(g_mq_mon);
    if (mq->usedCount == 0) {
        if (flags == OS_MESSAGE_NOBLOCK) return FALSE;
        g_mq_cv.wait(lk, [mq]{ return mq->usedCount > 0; });
    }
    if (msg) *(OSMessage*)msg = ((OSMessage*)mq->msgArray)[mq->firstIndex];
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    g_mq_cv.notify_all();
    return TRUE;
}

// ===========================================================================
// Heap / arena
//   OSAllocFromHeap is backed by aligned malloc (32-byte, GC default). JKRHeap
//   does the real sub-allocation atop a few big OSAlloc blocks, so this layer is
//   called rarely with large sizes. LANDMINE: allocations are NOT guaranteed to
//   fall within [OSGetArenaLo, OSGetArenaHi]; if the game ever assumes that, switch
//   to an arena bump/free-list allocator over the g_arena block.
// ===========================================================================
namespace {
void* aligned_alloc32(unsigned long size) {
    void* p = nullptr;
    if (size == 0) size = 1;
    if (posix_memalign(&p, 32, size) != 0) return nullptr;
    return p;
}
}

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int /*maxHeaps*/) {
    g_arena_lo = arenaStart;
    g_arena_hi = arenaEnd;
    return arenaStart;
}

int OSCreateHeap(void* /*start*/, void* /*end*/) { return g_next_heap++; }
void OSDestroyHeap(int /*heap*/) {}

void* OSAllocFromHeap(int /*heap*/, unsigned long size) {
    void* p = aligned_alloc32(size);
    if (p) { std::lock_guard<std::mutex> lk(g_heap_mu); g_referent[p] = size; }
    return p;
}
void OSFreeToHeap(int /*heap*/, void* ptr) {
    if (!ptr) return;
    { std::lock_guard<std::mutex> lk(g_heap_mu); g_referent.erase(ptr); }
    std::free(ptr);
}
unsigned long OSReferentSize(void* ptr) {
    std::lock_guard<std::mutex> lk(g_heap_mu);
    auto it = g_referent.find(ptr);
    return it == g_referent.end() ? 0 : it->second;
}
long OSCheckHeap(int /*heap*/) { return 0; }   // >=0 means consistent
void OSDumpHeap(int /*heap*/) {}

void* OSGetArenaLo(void) { return g_arena_lo; }
void* OSGetArenaHi(void) { return g_arena_hi; }
void  OSSetArenaLo(void* p) { g_arena_lo = p; }
void  OSSetArenaHi(void* p) { g_arena_hi = p; }

// ===========================================================================
// Stopwatch
// ===========================================================================
void OSInitStopwatch(OSStopwatch* sw, char* name) {
    sw->name = name; sw->total = 0; sw->hits = 0;
    sw->min = 0x7FFFFFFFFFFFFFFFLL; sw->max = 0; sw->last = 0; sw->running = 0;
}
void OSResetStopwatch(OSStopwatch* sw) {
    sw->total = 0; sw->hits = 0; sw->min = 0x7FFFFFFFFFFFFFFFLL;
    sw->max = 0; sw->last = 0;
}
void OSStartStopwatch(OSStopwatch* sw) { sw->running = 1; sw->last = now_ticks(); }
void OSStopStopwatch(OSStopwatch* sw) {
    if (!sw->running) return;
    long long elapsed = now_ticks() - sw->last;
    sw->running = 0; sw->last = elapsed; sw->total += elapsed; sw->hits++;
    if (elapsed < sw->min) sw->min = elapsed;
    if (elapsed > sw->max) sw->max = elapsed;
}
long long OSCheckStopwatch(OSStopwatch* sw) {
    return sw->running ? (sw->total + (now_ticks() - sw->last)) : sw->total;
}

// ===========================================================================
// Misc HW-vestigial
// ===========================================================================
void OSProtectRange(u32, void*, u32, u32) {}          // no MMU on host
void OSFillFPUContext(OSContext*) {}                  // no register-file swap
OSErrorHandler OSSetErrorHandler(OSError, OSErrorHandler old) { return old; }
void OSResetSystem(int, u32, BOOL) { g_reset_requested = true; }
BOOL OSGetResetSwitchState(void) { return FALSE; }
u32  OSGetSoundMode(void) { return g_sound_mode.load(); }
void OSSetSoundMode(u32 m) { g_sound_mode = m; }
u32  OSGetProgressiveMode(void) { return g_progressive_mode.load(); }
void OSSetProgressiveMode(u32 m) { g_progressive_mode = m; }

// Font — needs the SDK font ROM (sysmenu font). Not yet ported; stub returning "no
// glyph" so callers degrade gracefully. LANDMINE: OSReport-to-screen / debug font
// rendering will show nothing until OSInitFont reads real font data.
BOOL OSInitFont(OSFontHeader*) { return FALSE; }
u16  OSGetFontEncode(void) { return 0; /* ANSI */ }
char* OSGetFontTexture(char* str, void** image, s32* x, s32* y, s32* width) {
    if (image) *image = nullptr;
    if (x) *x = 0;
    if (y) *y = 0;
    if (width) *width = 0;
    return str && *str ? str + 1 : str;
}
char* OSGetFontWidth(char* str, s32* width) {
    if (width) *width = 0;
    return str && *str ? str + 1 : str;
}

// ---- cache ops (OSCache.h) ------------------------------------------------
// On GameCube these flush/invalidate the L1 data/instruction cache so the GPU
// (which reads main RAM directly, bypassing the CPU cache) sees CPU writes — and
// vice versa. On the native host there is ONE coherent address space and no
// separate GX FIFO DMA engine reading stale RAM, so flush/store/invalidate are
// no-ops. DCZeroRange is a real memset (callers use it to zero a buffer); it
// operates on whole 32-byte cache blocks, matching dcbz granularity.
void DCInvalidateRange(void*, u32) {}
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
void DCFlushRangeNoSync(void*, u32) {}
void DCStoreRangeNoSync(void*, u32) {}
void DCTouchRange(void*, u32) {}
void ICInvalidateRange(void*, u32) {}
void DCZeroRange(void* addr, u32 nBytes) {
    if (!addr || !nBytes) return;
    // Round to the 32-byte cache-block span the HW would clear.
    uintptr_t lo = (uintptr_t)addr & ~uintptr_t(31);
    uintptr_t hi = ((uintptr_t)addr + nBytes + 31u) & ~uintptr_t(31);
    std::memset((void*)lo, 0, hi - lo);
}

} // extern "C"
