// OWNED COPY of reference/sms/src/JSystem/JKernel/JKRThread.cpp — keep in sync.
// ONLY 64-bit-port change: two pointer-arithmetic sites cast host pointers
// through (u32): the stack-top `(void*)((u32)mStackMemory + mStackSize)` and the
// stack-size `(u32)stackEnd - (u32)stackBase`. Widened the intermediate to
// uintptr_t so the host pointers are not truncated on LP64 (the stack-top stays
// a void*, the size stays the same value). Verbatim otherwise.
#include <stdint.h>
#include <JSystem/JKernel/JKRMacro.hpp>
#include <JSystem/JKernel/JKRThread.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>

JSUList<JKRThread> JKRThread::sThreadList;

JKRThread::JKRThread(u32 stackSize, int msgCount, int threadPrio)
    : mLink(this)
{
	mHeap = JKRHeap::findFromRoot(this);
	if (mHeap == nullptr) {
		mHeap = JKRHeap::getSystemHeap();
	}

	mStackSize    = JKR_ALIGN32(stackSize);
	mStackMemory  = JKRHeap::alloc(mStackSize, 32, mHeap);
	mThreadRecord = JKRHeap::allocOne<OSThread>(32, mHeap);
	OSCreateThread(mThreadRecord, &JKRThread::start, this,
	               (void*)((uintptr_t)mStackMemory + mStackSize), mStackSize,
	               threadPrio, OS_THREAD_ATTR_DETACH);
	mMesgCount  = msgCount;
	mMesgBuffer = JKRHeap::allocArray<OSMessage>(mMesgCount, 0, mHeap);
	OSInitMessageQueue(&mMesgQueue, mMesgBuffer, mMesgCount);
	JKRThread::sThreadList.append(&mLink);
}

JKRThread::JKRThread(OSThread* threadRecord, int msgCount)
    : mLink(this)
{
	mHeap         = nullptr;
	mThreadRecord = threadRecord;
	mStackSize    = (u32)((uintptr_t)threadRecord->stackEnd
	                      - (uintptr_t)threadRecord->stackBase);
	mStackMemory  = threadRecord->stackBase;
	mMesgCount    = msgCount;
	mMesgBuffer   = (OSMessage*)JKRHeap::sSystemHeap->alloc(
        mMesgCount * sizeof(OSMessage), 4);
	OSInitMessageQueue(&mMesgQueue, mMesgBuffer, mMesgCount);
	JKRThread::sThreadList.append(&mLink);
}

JKRThread::~JKRThread()
{
	JKRThread::sThreadList.remove(&mLink);

	if (mHeap != nullptr) {
		if (!OSIsThreadTerminated(mThreadRecord)) {
			OSDetachThread(mThreadRecord);
			OSCancelThread(mThreadRecord);
		}

		JKRHeap::free(mStackMemory, mHeap);
		JKRHeap::free(mThreadRecord, mHeap);
	}

	JKRHeap::free(mMesgBuffer, nullptr);
}

void* JKRThread::start(void* thread)
{
	return static_cast<JKRThread*>(thread)->run();
}
