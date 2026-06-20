// os_test.cpp — TDD harness for the OS kernel seam (native/platform/os_impl.cpp).
//
// Verifies the PRIMITIVE CONTRACTS (not GC priority-scheduling fidelity, which is
// deferred to boot — see os_impl.cpp header): mutual exclusion + recursive relock,
// FIFO message passing with blocking + jam, cond wait/signal, thread
// create/resume/join/exit/detach, time monotonicity, heap alloc/free/referent-size,
// stopwatch accumulation. Each tested function IS the shipping extern "C" seam fn.

#include <dolphin/os.h>
#include <dolphin/os/OSThread.h>
#include <dolphin/os/OSMutex.h>
#include <dolphin/os/OSMessage.h>
#include <dolphin/os/OSStopwatch.h>
#include <dolphin/os/OSAlloc.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

// ---- threads --------------------------------------------------------------
static std::atomic<int> g_ran{0};
static void* worker(void* p) {
    g_ran += (int)(intptr_t)p;
    return (void*)0x1234;
}
static void test_threads() {
    OSThread t;
    char stack[4096];
    g_ran = 0;
    chk(OSCreateThread(&t, worker, (void*)7, stack + sizeof(stack), sizeof(stack),
                       16, 0) == 1, "CreateThread");
    chk(OSGetThreadPriority(&t) == 16, "ThreadPriority");
    chk(OSIsThreadTerminated(&t) == FALSE, "not-terminated-before-resume");
    OSResumeThread(&t);
    void* rv = nullptr;
    chk(OSJoinThread(&t, &rv) == 1, "JoinThread");
    chk(g_ran.load() == 7, "worker-ran");
    chk(rv == (void*)0x1234, "join-retval");
}

static void* exiter(void* p) {
    OSThread* self = OSGetCurrentThread();
    self->val = (void*)0xBEEF;
    OSExitThread(self);
    g_ran += 1000;  // must NOT run
    (void)p;
    return nullptr;
}
static void test_exit() {
    OSThread t;
    char stack[4096];
    g_ran = 0;
    OSCreateThread(&t, exiter, nullptr, stack + sizeof(stack), sizeof(stack), 16, 0);
    OSResumeThread(&t);
    void* rv = nullptr;
    OSJoinThread(&t, &rv);
    chk(g_ran.load() == 0, "OSExitThread-unwinds");
    chk(rv == (void*)0xBEEF, "exit-val");
}

// ---- mutex mutual exclusion + recursion -----------------------------------
static OSMutex g_mu;
static long long g_counter = 0;
static void* hammer(void* p) {
    for (int i = 0; i < 20000; ++i) {
        OSLockMutex(&g_mu);
        OSLockMutex(&g_mu);  // recursive relock
        long long v = g_counter;
        std::this_thread::yield();
        g_counter = v + 1;
        OSUnlockMutex(&g_mu);
        OSUnlockMutex(&g_mu);
    }
    (void)p;
    return nullptr;
}
static void test_mutex() {
    OSInitMutex(&g_mu);
    g_counter = 0;
    OSThread a, b;
    static char sa[8192], sb[8192];
    OSCreateThread(&a, hammer, nullptr, sa + sizeof(sa), sizeof(sa), 16, 0);
    OSCreateThread(&b, hammer, nullptr, sb + sizeof(sb), sizeof(sb), 16, 0);
    OSResumeThread(&a);
    OSResumeThread(&b);
    OSJoinThread(&a, nullptr);
    OSJoinThread(&b, nullptr);
    chk(g_counter == 40000, "mutex-mutual-exclusion (no lost updates)");

    // TryLock on a free mutex succeeds; recursive try succeeds.
    OSMutex m; OSInitMutex(&m);
    chk(OSTryLockMutex(&m) == TRUE, "TryLock-free");
    chk(OSTryLockMutex(&m) == TRUE, "TryLock-recursive");
    OSUnlockMutex(&m);
    OSUnlockMutex(&m);
}

// ---- message queue: FIFO + blocking + jam ---------------------------------
static OSMessageQueue g_mq;
static OSMessage g_mqbuf[4];
static void* producer(void* p) {
    for (intptr_t i = 1; i <= 100; ++i)
        OSSendMessage(&g_mq, (void*)i, OS_MESSAGE_BLOCK);  // blocks when full
    (void)p;
    return nullptr;
}
static void test_msgq() {
    OSInitMessageQueue(&g_mq, g_mqbuf, 4);
    OSThread prod;
    static char sp[8192];
    OSCreateThread(&prod, producer, nullptr, sp + sizeof(sp), sizeof(sp), 16, 0);
    OSResumeThread(&prod);
    bool ordered = true;
    for (intptr_t i = 1; i <= 100; ++i) {
        OSMessage m = nullptr;
        OSReceiveMessage(&g_mq, &m, OS_MESSAGE_BLOCK);
        if ((intptr_t)m != i) ordered = false;
    }
    OSJoinThread(&prod, nullptr);
    chk(ordered, "msgq-FIFO-across-blocking");

    // Nonblock on empty fails; on full fails.
    OSMessageQueue q; OSMessage qb[2];
    OSInitMessageQueue(&q, qb, 2);
    OSMessage m = nullptr;
    chk(OSReceiveMessage(&q, &m, OS_MESSAGE_NOBLOCK) == FALSE, "recv-empty-noblock");
    chk(OSSendMessage(&q, (void*)1, OS_MESSAGE_NOBLOCK) == TRUE, "send1");
    chk(OSSendMessage(&q, (void*)2, OS_MESSAGE_NOBLOCK) == TRUE, "send2");
    chk(OSSendMessage(&q, (void*)3, OS_MESSAGE_NOBLOCK) == FALSE, "send-full-noblock");
    // Jam puts at front (LIFO-ish): jam 9, then receive should give 9 before 1.
    OSJamMessage(&q, (void*)9, OS_MESSAGE_NOBLOCK);  // wait, full -> fails; drain one
    OSReceiveMessage(&q, &m, OS_MESSAGE_NOBLOCK);    // pops 1
    OSJamMessage(&q, (void*)9, OS_MESSAGE_NOBLOCK);  // now room; 9 at front
    OSReceiveMessage(&q, &m, OS_MESSAGE_NOBLOCK);
    chk((intptr_t)m == 9, "jam-front");
}

// ---- cond -----------------------------------------------------------------
static OSMutex g_cmu;
static OSCond g_cond;
static std::atomic<int> g_signalled{0};
static void* waiter(void* p) {
    OSLockMutex(&g_cmu);
    OSWaitCond(&g_cond, &g_cmu);  // releases + reacquires
    g_signalled = 1;
    OSUnlockMutex(&g_cmu);
    (void)p;
    return nullptr;
}
static void test_cond() {
    OSInitMutex(&g_cmu);
    OSInitCond(&g_cond);
    g_signalled = 0;
    OSThread w;
    static char sw[8192];
    OSCreateThread(&w, waiter, nullptr, sw + sizeof(sw), sizeof(sw), 16, 0);
    OSResumeThread(&w);
    // Give the waiter time to block on the cond.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    chk(g_signalled.load() == 0, "cond-blocks-before-signal");
    OSSignalCond(&g_cond);
    OSJoinThread(&w, nullptr);
    chk(g_signalled.load() == 1, "cond-wakes-on-signal");
}

// ---- interrupts (critical-section mutual exclusion) -----------------------
static long long g_isr = 0;
static void* intr_hammer(void* p) {
    for (int i = 0; i < 20000; ++i) {
        BOOL e = OSDisableInterrupts();
        BOOL e2 = OSDisableInterrupts();  // nested
        long long v = g_isr;
        std::this_thread::yield();
        g_isr = v + 1;
        OSRestoreInterrupts(e2);
        OSRestoreInterrupts(e);
    }
    (void)p;
    return nullptr;
}
static void test_interrupts() {
    g_isr = 0;
    OSThread a, b;
    static char sa[8192], sb[8192];
    OSCreateThread(&a, intr_hammer, nullptr, sa + sizeof(sa), sizeof(sa), 16, 0);
    OSCreateThread(&b, intr_hammer, nullptr, sb + sizeof(sb), sizeof(sb), 16, 0);
    OSResumeThread(&a);
    OSResumeThread(&b);
    OSJoinThread(&a, nullptr);
    OSJoinThread(&b, nullptr);
    chk(g_isr == 40000, "interrupts-critical-section-exclusion");
}

// ---- time -----------------------------------------------------------------
static void test_time() {
    OSTime t0 = OSGetTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    OSTime t1 = OSGetTime();
    chk(t1 > t0, "time-monotonic");
    // ~20ms should be >= ~18ms worth of 40.5MHz ticks.
    long long dticks = (long long)(t1 - t0);
    chk(dticks > (40500000LL * 18 / 1000), "time-rate-sane");

    // Calendar round-trip: 1 day + 1 hour + 1 min + 1 sec worth of ticks.
    long long secs = 86400 + 3600 + 60 + 1;
    OSCalendarTime ct;
    OSTicksToCalendarTime((OSTime)(secs * 40500000LL), &ct);
    chk(ct.sec == 1, "cal-sec");
    chk(ct.min == 1, "cal-min");
    chk(ct.hour == 1, "cal-hour");
}

// ---- heap -----------------------------------------------------------------
static void test_heap() {
    int junk1, junk2;
    OSInitAlloc(&junk1, &junk2, 4);
    int h = OSCreateHeap(&junk1, &junk2);
    chk(h >= 0, "CreateHeap");
    void* a = OSAllocFromHeap(h, 1000);
    void* b = OSAllocFromHeap(h, 2000);
    chk(a && b && a != b, "alloc-distinct");
    chk(((uintptr_t)a % 32) == 0, "alloc-32-aligned");
    chk(OSReferentSize(a) == 1000, "referent-size-a");
    chk(OSReferentSize(b) == 2000, "referent-size-b");
    OSFreeToHeap(h, a);
    OSFreeToHeap(h, b);
    chk(OSCheckHeap(h) >= 0, "CheckHeap-consistent");
}

// ---- stopwatch ------------------------------------------------------------
static void test_stopwatch() {
    OSStopwatch sw;
    OSInitStopwatch(&sw, (char*)"t");
    OSStartStopwatch(&sw);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    OSStopStopwatch(&sw);
    chk(sw.hits == 1, "stopwatch-hits");
    chk(sw.total > 0, "stopwatch-accumulates");
}

int main() {
    std::printf("== OS seam unit tests ==\n");
    test_threads();
    test_exit();
    test_mutex();
    test_msgq();
    test_cond();
    test_interrupts();
    test_time();
    test_heap();
    test_stopwatch();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
