// os_seam.h — native reimplementation of the GC OS kernel API.
//
// SDK headers replaced: reference/sms/include/dolphin/os/*.h, os.h
// Used surface (decomp grep): ~195 distinct, 1439 calls. Hot: OSRestoreInterrupts
// (183), OSDisableInterrupts(146), OSReport(112), OSSendMessage(29), OSGetTime(28),
// OSReceiveMessage/OSInitMessageQueue(24 each), OSUnlockMutex(21), OSCreateThread(18).
//
// MAPPING (see README.md "OS"):
//   threads  -> host std::thread (OR a cooperative fiber scheduler; see note)
//   mutex/cond -> std::mutex / std::condition_variable
//   message queue -> bounded blocking queue
//   interrupts -> a recursive global "OS critical-section" lock (cheap; hot path)
//   time/alarm -> host monotonic clock; ticks = fixed-rate counter (OS_TIME_SPEED)
//   heap/arena -> hand JKR one big native block; OSAlloc over malloc
//   cache (DC/IC) -> no-ops (coherent host); LC -> malloc scratch
//   context/font/reset/rtc -> host-native (panic=abort, report=printf, rtc=host clock)
//
// IMPORTANT design choice for phase-2 lead: GC threads use a fixed-priority
// cooperative-ish model and the engine relies on OSDisableInterrupts critical
// sections + explicit OSYieldThread. Two viable backends:
//   (A) preemptive host threads + a global OS lock standing in for "interrupts
//       disabled". Simplest; matches std primitives 1:1. Risk: the engine assumes
//       non-preemption inside a disabled-interrupt window — the global lock must be
//       held for the *whole* window so no other game thread runs meanwhile.
//   (B) cooperative scheduler (one host thread, fibers). Faithful to GC semantics,
//       no data races, but more work. The recomp build's docs/native_threading.md
//       has prior art (token+condvar substrate) — read it before choosing.
// Recommendation: start with (A) + a fat global lock, migrate hot contention later.
//
// The structs below keep the SDK's opaque-handle shape so the game's headers
// (which embed e.g. OSThread* fields in objects) keep their layout. Internally
// each handle carries a pointer to the native backing object.
#pragma once
#include "platform_types.h"

namespace sb::platform::os {

// ---- bring-up ------------------------------------------------------------
void Init();                 // OSInit: arena, default heap, clock base, main thread.
[[noreturn]] void Panic(const char* file, int line, const char* msg); // OSPanic
void Report(const char* fmt, ...);                                    // OSReport

// ---- threads (os/OSThread.h) --------------------------------------------
struct Thread;               // opaque; backs the SDK OSThread storage
using ThreadFunc = void* (*)(void*);
// TODO: OSCreateThread(thread, func, param, stackTop, stackSize, prio, flags)
bool   CreateThread(Thread* t, ThreadFunc fn, void* param, void* stackTop,
                    u32 stackSize, s32 prio, u16 flags);
long   ResumeThread(Thread* t);     // TODO
s32    SuspendThread(Thread* t);     // TODO
[[noreturn]] void ExitThread(Thread* t, void* val);  // TODO: OSExitThread
bool   JoinThread(Thread* t, void** valOut);  // TODO
void   DetachThread(Thread* t);   // TODO
void   CancelThread(Thread* t);    // TODO
void   YieldThread();              // TODO: OSYieldThread
Thread* GetCurrentThread();        // TODO
s32    GetThreadPriority(Thread* t);            // TODO
bool   SetThreadPriority(Thread* t, s32 prio);  // TODO
s32    DisableScheduler();   // TODO
s32    EnableScheduler();    // TODO

// thread-queue sleep/wake (os/OSThread.h)
struct ThreadQueue;          // opaque
void InitThreadQueue(ThreadQueue* q);  // TODO
void SleepThread(ThreadQueue* q);      // TODO: block current on q
void WakeupThread(ThreadQueue* q);     // TODO: wake all on q

// ---- mutex / cond (os/OSMutex.h) ----------------------------------------
struct Mutex;  struct Cond;
void InitMutex(Mutex* m);    void LockMutex(Mutex* m);   // TODO (recursive: GC mutex is recursive)
void UnlockMutex(Mutex* m);  bool TryLockMutex(Mutex* m);// TODO
void InitCond(Cond* c);  void WaitCond(Cond* c, Mutex* m);  void SignalCond(Cond* c); // TODO

// ---- message queue (os/OSMessage.h) -------------------------------------
struct MessageQueue;  using Message = void*;
enum MsgFlags : s32 { MsgNoBlock = 0, MsgBlock = 1 };
void InitMessageQueue(MessageQueue* mq, Message* buf, s32 capacity);   // TODO
bool SendMessage(MessageQueue* mq, Message m, s32 flags);              // TODO
bool ReceiveMessage(MessageQueue* mq, Message* out, s32 flags);        // TODO
bool JamMessage(MessageQueue* mq, Message m, s32 flags);               // TODO

// ---- interrupts / critical sections (os/OSInterrupt.h) ------------------
// On HW these toggle MSR[EE]; natively they enter/leave a recursive global OS lock
// so a "disabled-interrupts" window is mutually exclusive with every game thread.
// Returns the prior enable state (the SDK contract).
bool DisableInterrupts();             // TODO: enter critical section, return prev
bool EnableInterrupts();              // TODO
bool RestoreInterrupts(bool prev);    // TODO: leave/restore to prev

// ---- time / alarm (os/OSTime.h, OSAlarm.h) ------------------------------
// OSTime is a 64-bit tick count at OS_TIME_SPEED (= bus clock / 4). Map to a host
// monotonic clock scaled to that rate so the OS_*_TO_TICKS macros stay valid.
u64  GetTime();   // TODO: OSGetTime
u32  GetTick();   // TODO: OSGetTick (low 32)
struct Alarm;  using AlarmHandler = void (*)(Alarm*, void* /*context*/);
void CreateAlarm(Alarm* a);                                  // TODO
void SetAlarm(Alarm* a, u64 ticksFromNow, AlarmHandler h);   // TODO
void CancelAlarm(Alarm* a);                                  // TODO

// ---- heap / arena (os/OSAlloc.h, OSMemory.h) ----------------------------
// JKRHeap sits on top of these. The arena seam just provides one large native
// block; OSAlloc/OSFree wrap host malloc/free for the rare direct callers.
void* GetArenaLo();   void* GetArenaHi();   // TODO: bounds of the native arena
void  InitAlloc(void* arenaLo, void* arenaHi, s32 maxHeaps); // TODO
void* AllocFromArenaLo(u32 size, u32 align); // TODO
void* AllocFromArenaHi(u32 size, u32 align); // TODO

// ---- cache (os/OSCache.h, OSLC.h) ---------------------------------------
// Host CPU caches are coherent w.r.t. our renderer/audio (we read native structs,
// not a guest RAM aperture), so DC/IC range ops are no-ops. LC (locked cache) is
// used by THP as DMA scratch -> back it with a malloc'd buffer.
inline void DCFlushRange(void*, u32) {}        // no-op
inline void DCInvalidateRange(void*, u32) {}   // no-op
inline void DCStoreRange(void*, u32) {}        // no-op
inline void ICInvalidateRange(void*, u32) {}   // no-op
void* LCAlloc(u32 size);     // TODO: scratch buffer for THP
void  LCFree(void* p);       // TODO

// ---- misc HW-vestigial (context / reset / rtc / font) -------------------
// OSContext save/restore is mostly vestigial under host threads (no register-file
// to swap); keep as no-op/trivial stubs unless a caller genuinely round-trips one.
// OSReport already above. OSResetSystem -> request app quit. Font tables ship from
// SDK data. RTC -> host wall clock + an EXI-SRAM-backed settings file (see exi_seam).
// Phase-2: implement the few that the boot path actually reaches; stub the rest LOUD.

} // namespace sb::platform::os
