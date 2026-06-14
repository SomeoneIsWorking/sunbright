// ===========================================================================
// port/pal/os/os_thread.cpp — REAL native cooperative OS-thread scheduler for
// the PC port. Step 2 of the runtime bring-up (heap -> *OS/threads* -> teardown
// -> boot -> render -> input).
//
// Replaces the link-only thread/message/mutex STUBS (formerly in
// os_thread_heap_stub.cpp's Threads/Messages sections and os_sync.cpp) with a
// faithful implementation so the engine's worker threads (JKRDecomp, JKRAram,
// JASDvd, the audio/DVD workers) actually RUN, block, and hand off work.
//
// THE EXECUTION MODEL — single logical CPU, cooperative
// ----------------------------------------------------
// The GameCube OS is a *cooperative* priority scheduler: exactly one thread
// runs at a time, and a context switch happens ONLY at an explicit block point
// (OSSleepThread, an empty/full blocking message queue, a contended mutex, a
// join/suspend, or a yield). There is no involuntary preemption of engine code.
//
// We reproduce that EXACTLY on the host. Each OS thread is a real std::thread,
// but only ONE is ever runnable at a time — `g_current`. Every other thread is
// parked in a condition_variable wait. A thread that blocks marks itself
// non-runnable, hands the CPU to the highest-priority ready thread, and parks
// until it is made current again. Engine code therefore runs with the SAME
// "one thread at a time, switch only at block points" guarantee the game shipped
// with — there are provably NO new interleavings vs the console, so no new race
// surface (the lesson the recompiler track learned the hard way: preemptive host
// threads expose data races GC code never had; cooperative does not).
//
// The kernel lock `g_klock` is held ONLY transiently inside the OS primitives
// while they touch scheduler state; it is NOT held across engine code. Mutual
// exclusion of engine code is guaranteed by the invariant "all threads except
// g_current are parked", not by holding a lock across arbitrary work (which
// would deadlock the moment engine code re-entered a primitive).
//
// WHY THIS MODEL (the two requirements that drove it):
//   1. NO RACE CONDITIONS. Because at most one thread ever touches engine state
//      (the others are parked), there is no concurrent access — the data-race
//      class cannot occur in engine code. We get real worker threads WITHOUT the
//      shared-memory hazards preemptive host threads would introduce.
//   2. OS-PORTABLE. Built only from standard C++ (std::thread / std::mutex /
//      std::condition_variable). No OS-specific threading APIs, no ucontext, no
//      per-platform #ifdef — identical behaviour on Linux / macOS / Windows and
//      on arm64. "threading behaves differently per OS" does not apply here.
//   It adds NO deadlock risk beyond the console: a thread can only block waiting
//   for something that, on hardware, also had to be delivered by another thread.
//
// State lives where it does on hardware: the OSThread struct keeps its GC layout
// (state/suspend/priority/link/queueJoin are read/written here and by the
// engine); host-only bits (the std::thread, the per-thread parking condvar, the
// entry func/param, the exit value) live in a side table keyed by OSThread*.
//
// BRING-UP LIMITATIONS (flagged honestly):
//   - OSCancelThread of a still-parked thread leaks its host std::thread (the
//     parked thread is never woken to unwind). Engine worker threads are
//     long-lived and not cancelled in the happy path; revisit if a real cancel
//     path appears.
//   - HostThread side entries are not reclaimed when the engine frees an
//     OSThread record (JKRThread::~ frees mThreadRecord). Worker threads live
//     for the whole session, so the set is small and bounded. Revisit with a
//     real teardown story (the next bring-up step).
// ===========================================================================

#include <dolphin/os.h>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// THE single kernel lock. Held only inside primitives, never across engine code.
std::mutex g_klock;

// Host-only per-thread state, keyed by OSThread* (the GC struct keeps its layout).
struct HostThread {
	OSThread* os = nullptr;
	std::thread th;                  // empty for the bootstrap (main) thread
	std::condition_variable cv;      // parked here while not the running thread
	void* (*func)(void*) = nullptr;
	void* param = nullptr;
	void* retval = nullptr;
	bool detached = false;
};

std::vector<HostThread*> g_all;      // all registered threads (guarded by g_klock)
OSThread* g_current = nullptr;       // the one thread allowed to run engine code
thread_local OSThread* t_self = nullptr;

HostThread* host_of(OSThread* t) {
	for (HostThread* h : g_all)
		if (h->os == t) return h;
	return nullptr;
}

// Register the calling (process) thread as the bootstrap "main" OS thread the
// first time any primitive runs, so OSGetCurrentThread works and main can block.
// Only ever hit by the bootstrap thread: worker threads set t_self/g_current in
// their trampoline before running any engine code.
void ensure_bootstrap() {
	if (g_current) return;
	static OSThread s_main;
	std::memset(&s_main, 0, sizeof(s_main));
	s_main.state = OS_THREAD_STATE_RUNNING;
	s_main.priority = 16;            // mid (0=highest .. 31=lowest)
	s_main.base = 16;
	HostThread* h = new HostThread();
	h->os = &s_main;
	g_all.push_back(h);
	g_current = &s_main;
	t_self = &s_main;
}

// ---- OSThreadQueue intrusive wait-queue (uses OSThread::link) ----
// A thread waits on at most one queue at a time, so the single `link` suffices.
void q_enqueue(OSThreadQueue* q, OSThread* t) {
	t->queue = q;
	t->link.next = nullptr;
	t->link.prev = q->tail;
	if (q->tail) q->tail->link.next = t; else q->head = t;
	q->tail = t;
}
OSThread* q_dequeue(OSThreadQueue* q) {
	OSThread* t = q->head;
	if (!t) return nullptr;
	q->head = t->link.next;
	if (q->head) q->head->link.prev = nullptr; else q->tail = nullptr;
	t->queue = nullptr;
	t->link.next = t->link.prev = nullptr;
	return t;
}
void q_remove(OSThreadQueue* q, OSThread* t) {
	if (t->queue != q) return;
	OSThread* p = t->link.prev;
	OSThread* n = t->link.next;
	if (p) p->link.next = n; else q->head = n;
	if (n) n->link.prev = p; else q->tail = p;
	t->queue = nullptr;
	t->link.next = t->link.prev = nullptr;
}

// Highest-priority runnable thread (lowest priority number), excluding the
// running one (RUNNING threads are not READY). nullptr if nothing is runnable.
OSThread* pick_next() {
	OSThread* best = nullptr;
	for (HostThread* h : g_all) {
		OSThread* t = h->os;
		if (t->state == OS_THREAD_STATE_READY && t->suspend <= 0) {
			if (!best || t->priority < best->priority) best = t;
		}
	}
	return best;
}

// Hand the CPU to the next runnable thread and BLOCK until *self* is current
// again. The caller holds `lk` and has already moved self off the runnable set
// (state WAITING/MORIBUND, or suspend>0). When self resumes it is RUNNING.
void switch_away(std::unique_lock<std::mutex>& lk) {
	OSThread* self = t_self;
	OSThread* nxt = pick_next();
	g_current = nxt;                 // may be null: a later make-runnable resumes us
	if (nxt) host_of(nxt)->cv.notify_all();
	HostThread* h = host_of(self);
	h->cv.wait(lk, [&] { return g_current == self; });
	self->state = OS_THREAD_STATE_RUNNING;
}

// Cooperative reschedule point: if a runnable thread now outranks the caller,
// yield the CPU to it (the caller stays runnable and is re-picked later). This
// is the priority-preemption-at-a-reschedule-point the GC OSWakeup/Resume/Send
// primitives perform. Caller holds `lk`.
void yield_to_higher(std::unique_lock<std::mutex>& lk) {
	OSThread* self = t_self;
	OSThread* best = pick_next();
	if (best && best->priority < self->priority) {
		self->state = OS_THREAD_STATE_READY;
		switch_away(lk);
	}
}

void wake_all(OSThreadQueue* q) {
	while (OSThread* t = q_dequeue(q))
		t->state = OS_THREAD_STATE_READY;
}

// Host-thread entry: park until scheduled, run the engine entry func, then exit.
void start_trampoline(OSThread* self) {
	void* (*func)(void*);
	void* param;
	{
		std::unique_lock<std::mutex> lk(g_klock);
		t_self = self;
		HostThread* h = host_of(self);
		func = h->func;
		param = h->param;
		h->cv.wait(lk, [&] { return g_current == self; });
		self->state = OS_THREAD_STATE_RUNNING;
	}
	void* r = func(param);           // engine code; runs lock-free, single-runner
	host_of(self)->retval = r;       // safe: single runner; surfaced via OSJoinThread
	OSExitThread(self);              // never returns to here past the switch
}

}  // namespace

extern "C" {

// --------------------------------------------------------------- Thread queues
void OSInitThreadQueue(OSThreadQueue* queue) {
	if (queue) { queue->head = nullptr; queue->tail = nullptr; }
}

// --------------------------------------------------------------- Threads
int OSCreateThread(OSThread* thread, void* (*func)(void*), void* param,
                   void* stack, u32 stackSize, s32 priority, u16 attr) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	std::memset(thread, 0, sizeof(*thread));
	thread->state = OS_THREAD_STATE_READY;
	thread->attr = attr;
	thread->suspend = 1;             // created suspended; OSResumeThread starts it
	thread->priority = priority;
	thread->base = priority;
	thread->stackBase = static_cast<u8*>(stack);
	thread->stackEnd = reinterpret_cast<u32*>(
	    static_cast<u8*>(stack) - stackSize);  // grows down; informational only
	OSInitThreadQueue(&thread->queueJoin);

	HostThread* h = new HostThread();
	h->os = thread;
	h->func = func;
	h->param = param;
	h->detached = (attr & OS_THREAD_ATTR_DETACH) != 0;
	g_all.push_back(h);
	h->th = std::thread(start_trampoline, thread);
	h->th.detach();                  // lifetime is cooperative (MORIBUND+queueJoin)
	return 1;
}

void OSExitThread(OSThread* thread) {
	std::unique_lock<std::mutex> lk(g_klock);
	OSThread* self = thread ? thread : t_self;
	HostThread* h = host_of(self);
	if (h) self->val = h->retval;    // (retval set via the implicit start return = null)
	self->state = OS_THREAD_STATE_MORIBUND;
	wake_all(&self->queueJoin);      // release joiners
	// Hand off the CPU permanently — do NOT wait to become current again.
	OSThread* nxt = pick_next();
	g_current = nxt;
	if (nxt) host_of(nxt)->cv.notify_all();
	// host std::thread returns from start_trampoline and ends (already detached).
}

void OSCancelThread(OSThread* thread) {
	std::unique_lock<std::mutex> lk(g_klock);
	if (!thread) return;
	if (thread->queue) q_remove(thread->queue, thread);
	thread->state = OS_THREAD_STATE_MORIBUND;
	wake_all(&thread->queueJoin);
	// A still-parked cancelled thread leaks its host thread (see header).
	yield_to_higher(lk);
}

void OSDetachThread(OSThread* thread) {
	std::unique_lock<std::mutex> lk(g_klock);
	thread->attr |= OS_THREAD_ATTR_DETACH;
	if (HostThread* h = host_of(thread)) h->detached = true;
}

int OSJoinThread(OSThread* thread, void* val) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	while (thread->state != OS_THREAD_STATE_MORIBUND) {
		OSThread* self = t_self;
		q_enqueue(&thread->queueJoin, self);
		self->state = OS_THREAD_STATE_WAITING;
		switch_away(lk);
	}
	if (val) {
		HostThread* h = host_of(thread);
		*static_cast<void**>(val) = h ? h->retval : thread->val;
	}
	return 1;
}

long OSResumeThread(OSThread* thread) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	long old = thread->suspend;
	if (thread->suspend > 0) {
		thread->suspend--;
		if (thread->suspend == 0 &&
		    thread->state != OS_THREAD_STATE_MORIBUND) {
			thread->state = OS_THREAD_STATE_READY;
			yield_to_higher(lk);     // a higher-priority resumed thread runs now
		}
	}
	return old;
}

s32 OSSuspendThread(OSThread* thread) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	s32 old = thread->suspend;
	thread->suspend++;
	if (thread == t_self && thread->suspend > 0) {
		// Suspending self: yield the CPU (we are no longer runnable until resumed).
		switch_away(lk);
	}
	return old;
}

void OSSleepThread(OSThreadQueue* queue) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	OSThread* self = t_self;
	q_enqueue(queue, self);
	self->state = OS_THREAD_STATE_WAITING;
	switch_away(lk);
}

void OSWakeupThread(OSThreadQueue* queue) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	wake_all(queue);
	yield_to_higher(lk);
}

void OSYieldThread(void) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	OSThread* self = t_self;
	OSThread* best = pick_next();
	if (best && best->priority <= self->priority) {  // round-robin equals too
		self->state = OS_THREAD_STATE_READY;
		switch_away(lk);
	}
}

BOOL OSIsThreadTerminated(OSThread* thread) {
	std::unique_lock<std::mutex> lk(g_klock);
	return thread && thread->state == OS_THREAD_STATE_MORIBUND;
}

OSThread* OSGetCurrentThread(void) { return t_self; }

long OSGetThreadPriority(OSThread* thread) {
	return thread ? thread->priority : 0;
}

// The cooperative kernel never preempts engine code involuntarily, so a
// "disable scheduler" region is already honored — there is no reschedule except
// at the explicit block points the engine itself reaches. No-op, faithful.
s32 OSDisableScheduler(void) { return 0; }
s32 OSEnableScheduler(void) { return 0; }

// --------------------------------------------------------------- Messages
void OSInitMessageQueue(OSMessageQueue* mq, void* msgArray, long msgCount) {
	if (!mq) return;
	std::memset(mq, 0, sizeof(*mq));
	mq->msgArray = msgArray;
	mq->msgCount = msgCount;
}

int OSSendMessage(OSMessageQueue* mq, void* msg, long flags) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	if (!mq || !mq->msgArray || mq->msgCount <= 0) return 0;
	while (mq->usedCount >= mq->msgCount) {
		if (!(flags & OS_MESSAGE_BLOCK)) return 0;        // full, non-blocking
		OSThread* self = t_self;
		q_enqueue(&mq->queueSend, self);
		self->state = OS_THREAD_STATE_WAITING;
		switch_away(lk);
	}
	long idx = (mq->firstIndex + mq->usedCount) % mq->msgCount;
	static_cast<OSMessage*>(mq->msgArray)[idx] = msg;
	mq->usedCount++;
	wake_all(&mq->queueReceive);                          // a waiting receiver can go
	yield_to_higher(lk);
	return 1;
}

int OSJamMessage(OSMessageQueue* mq, void* msg, long flags) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	if (!mq || !mq->msgArray || mq->msgCount <= 0) return 0;
	while (mq->usedCount >= mq->msgCount) {
		if (!(flags & OS_MESSAGE_BLOCK)) return 0;
		OSThread* self = t_self;
		q_enqueue(&mq->queueSend, self);
		self->state = OS_THREAD_STATE_WAITING;
		switch_away(lk);
	}
	mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
	static_cast<OSMessage*>(mq->msgArray)[mq->firstIndex] = msg;
	mq->usedCount++;
	wake_all(&mq->queueReceive);
	yield_to_higher(lk);
	return 1;
}

int OSReceiveMessage(OSMessageQueue* mq, void* msg, long flags) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	if (!mq || !mq->msgArray || mq->msgCount <= 0) return 0;
	while (mq->usedCount <= 0) {
		if (!(flags & OS_MESSAGE_BLOCK)) return 0;        // empty, non-blocking
		OSThread* self = t_self;
		q_enqueue(&mq->queueReceive, self);
		self->state = OS_THREAD_STATE_WAITING;
		switch_away(lk);
	}
	if (msg)
		*static_cast<OSMessage*>(msg) =
		    static_cast<OSMessage*>(mq->msgArray)[mq->firstIndex];
	mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
	mq->usedCount--;
	wake_all(&mq->queueSend);                             // a blocked sender can go
	yield_to_higher(lk);
	return 1;
}

// --------------------------------------------------------------- Mutexes
// Real recursive, blocking mutexes (moved here from os_sync.cpp — they need the
// scheduler to block a contended waiter). Uncontended acquire/recurse never
// blocks, so the single-threaded heap/boot path pays nothing.
void OSInitMutex(OSMutex* mutex) {
	if (mutex) std::memset(mutex, 0, sizeof(*mutex));
}

void OSLockMutex(OSMutex* mutex) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	OSThread* self = t_self;
	if (mutex->thread == self) { mutex->count++; return; }  // recursive re-lock
	while (mutex->count != 0) {                             // owned by another
		q_enqueue(&mutex->queue, self);
		self->state = OS_THREAD_STATE_WAITING;
		switch_away(lk);
	}
	mutex->thread = self;
	mutex->count = 1;
}

void OSUnlockMutex(OSMutex* mutex) {
	std::unique_lock<std::mutex> lk(g_klock);
	if (mutex->count == 0) return;
	if (--mutex->count == 0) {
		mutex->thread = nullptr;
		if (OSThread* w = q_dequeue(&mutex->queue))
			w->state = OS_THREAD_STATE_READY;
		yield_to_higher(lk);
	}
}

BOOL OSTryLockMutex(OSMutex* mutex) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	OSThread* self = t_self;
	if (mutex->thread == self) { mutex->count++; return TRUE; }
	if (mutex->count == 0) { mutex->thread = self; mutex->count = 1; return TRUE; }
	return FALSE;
}

// --------------------------------------------------------------- Cond vars
void OSInitCond(OSCond* cond) {
	if (cond) { cond->queue.head = nullptr; cond->queue.tail = nullptr; }
}

void OSWaitCond(OSCond* cond, OSMutex* mutex) {
	std::unique_lock<std::mutex> lk(g_klock);
	ensure_bootstrap();
	// Release the mutex fully, sleep on the cond, re-acquire on wake (GC semantics).
	s32 saved = mutex->count;
	mutex->count = 0;
	mutex->thread = nullptr;
	if (OSThread* w = q_dequeue(&mutex->queue))
		w->state = OS_THREAD_STATE_READY;
	OSThread* self = t_self;
	q_enqueue(&cond->queue, self);
	self->state = OS_THREAD_STATE_WAITING;
	switch_away(lk);
	// Re-acquire the mutex (block if needed), restoring the recursion depth.
	while (mutex->count != 0 && mutex->thread != self) {
		q_enqueue(&mutex->queue, self);
		self->state = OS_THREAD_STATE_WAITING;
		switch_away(lk);
	}
	mutex->thread = self;
	mutex->count = saved;
}

void OSSignalCond(OSCond* cond) {
	std::unique_lock<std::mutex> lk(g_klock);
	wake_all(&cond->queue);
	yield_to_higher(lk);
}

}  // extern "C"
