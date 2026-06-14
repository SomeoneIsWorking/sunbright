// Runtime test for the native cooperative OS-thread scheduler (os_thread.cpp),
// step 2 of the runtime bring-up. Proves the PRIMITIVES actually run threads and
// hand off work (the old stubs spawned nothing) AND that the real engine thread
// wrapper (port/src/JKRThread.cpp, compiled into core) drives a worker end-to-end.
//
// Stage A — raw OS primitives:
//   main OSCreateThread+OSResumeThread a worker that bumps a counter and posts a
//   "done" message back through a queue, then returns a value. main blocks in
//   OSReceiveMessage until the worker posts (proves cross-thread BLOCKING handoff
//   — impossible with the non-blocking stub), then OSJoinThread collects the
//   worker's return value (proves cooperative join + MORIBUND).
//
// Stage B — engine wrapper (JKRThread):
//   a JKRThread subclass whose run() loops on waitMeessageBlock(), doubling each
//   value it receives and posting the result to a reply queue, until a sentinel
//   tells it to exit. Proves the real decomp thread/message path runs natively
//   on top of the PAL scheduler + heap.
#include <JSystem/JKernel/JKRThread.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <dolphin/os.h>

#include <cstdio>
#include <cstdint>

extern "C" void sb_heap_bringup();

static int g_fail = 0;
#define CHECK(cond, msg) do { \
	if (cond) { std::printf("[os_thread_run]   ok: %s\n", msg); } \
	else      { std::printf("[os_thread_run] FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

// --------------------------------------------------------------------- Stage A
static volatile int g_counter = 0;
static OSMessageQueue g_doneq;
static OSMessage      g_donebuf[2];

static void* worker_a(void* param) {
	g_counter += 7;
	OSSendMessage(&g_doneq, (OSMessage)0xD09E, OS_MESSAGE_BLOCK);  // notify main
	return (void*)((uintptr_t)param + 100);                       // join return value
}

static void stage_a() {
	std::printf("[os_thread_run] --- Stage A: raw OS primitives ---\n");
	OSInitMessageQueue(&g_doneq, g_donebuf, 2);

	static OSThread t;
	static unsigned char stack[64 * 1024];
	int created = OSCreateThread(&t, worker_a, (void*)42,
	                             stack + sizeof(stack), sizeof(stack),
	                             17 /*lower prio than main(16)*/, OS_THREAD_ATTR_DETACH);
	CHECK(created == 1, "OSCreateThread returned success");
	CHECK(g_counter == 0, "worker has NOT run before resume (created suspended)");

	OSResumeThread(&t);

	OSMessage got = nullptr;
	int r = OSReceiveMessage(&g_doneq, &got, OS_MESSAGE_BLOCK);   // blocks until worker posts
	CHECK(r == 1 && got == (OSMessage)0xD09E, "blocking receive got the worker's message");
	CHECK(g_counter == 7, "worker body actually executed (counter bumped)");

	void* jr = nullptr;
	OSJoinThread(&t, &jr);
	CHECK(OSIsThreadTerminated(&t), "worker terminated after join");
	CHECK(jr == (void*)142, "OSJoinThread returned the worker's return value (42+100)");
}

// --------------------------------------------------------------------- Stage B
static OSMessageQueue g_replyq;
static OSMessage      g_replybuf[8];

struct EchoThread : public JKRThread {
	EchoThread() : JKRThread(64 * 1024, 8, 18) {}   // 64KB stack, 8 msgs, prio 18
	void* run() override {
		int handled = 0;
		for (;;) {
			OSMessage m = waitMeessageBlock();          // blocks until main sends
			if (m == (OSMessage)(uintptr_t)-1)           // sentinel: exit
				return (void*)(uintptr_t)handled;
			uintptr_t v = (uintptr_t)m;
			OSSendMessage(&g_replyq, (OSMessage)(v * 2), OS_MESSAGE_BLOCK);
			handled++;
		}
	}
};

static void stage_b() {
	std::printf("[os_thread_run] --- Stage B: JKRThread engine wrapper ---\n");
	OSInitMessageQueue(&g_replyq, g_replybuf, 8);

	EchoThread* th = new EchoThread();
	CHECK(th != nullptr, "EchoThread constructed (engine ctor: heap alloc + OSCreateThread)");
	th->resume();

	const uintptr_t inputs[] = { 3, 10, 21 };
	for (uintptr_t v : inputs)
		th->sendMessage((OSMessage)v);                  // NOBLOCK enqueue into worker

	bool all_ok = true;
	for (uintptr_t v : inputs) {
		OSMessage reply = nullptr;
		OSReceiveMessage(&g_replyq, &reply, OS_MESSAGE_BLOCK);  // blocks -> worker runs
		if ((uintptr_t)reply != v * 2) all_ok = false;
	}
	CHECK(all_ok, "JKRThread doubled every value across the thread boundary");

	th->sendMessage((OSMessage)(uintptr_t)-1);          // tell worker to exit
	void* jr = nullptr;
	OSJoinThread(th->getThreadRecord(), &jr);
	CHECK(jr == (void*)(uintptr_t)3, "EchoThread processed 3 values then exited cleanly");
}

int main() {
	std::setbuf(stdout, nullptr);
	sb_heap_bringup();   // engine ctor (JKRThread) allocates its stack/record from the heap
	stage_a();
	stage_b();
	if (g_fail) { std::printf("[os_thread_run] RESULT: FAIL\n"); return 1; }
	std::printf("[os_thread_run] RESULT: PASS — native cooperative threads run + hand off work\n");
	_Exit(0);   // skip static teardown (separate bring-up step; see heap_run)
}
