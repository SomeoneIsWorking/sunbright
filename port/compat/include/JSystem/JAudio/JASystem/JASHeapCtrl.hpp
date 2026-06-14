// SHADOW HEADER (64-bit port) of
// reference/sms/include/JSystem/JAudio/JASystem/JASHeapCtrl.hpp — keep in sync.
// 64-bit-port change: THeap::initMotherHeap's 1st param is the HEAP BASE host
// pointer (the body stores `unk8 = (u8*)ALIGN_NEXT(param_1, 0x20)`, and unk8 is
// a u8*) — JASWaveArcLoader passes the (void*) result of allocFromSysAramFull.
// Widened that param from u32 to uintptr_t; the 2nd param (heap size) stays u32.
// PAL ITEM: the THeap::initMotherHeap definition (JASHeapCtrl.cpp — not yet
// ported into the core) must match this widened signature when added.
// Verbatim otherwise.
#ifndef JASHEAPCTRL_HPP
#define JASHEAPCTRL_HPP

#include <dolphin/types.h>
#include <stdint.h>

namespace JASystem {

namespace Kernel {
	class THeap {
	public:
		THeap();
		~THeap() { }

		void init();
		void* alloc(THeap*, u32);
		BOOL allocHeapCheck(THeap*, u32);
		BOOL free();
		BOOL selfAlloc(THeap*, u32, u32);
		void initMotherHeap(uintptr_t, u32, u8);
		void* getUnlockHeap();
		void checkAlloc();
		void selfInit(u32, u32, u32);
		void setGroupHeap(THeap*);
		void cutDown();
		void garbageCollectionSt();
		void checkFreeHeapTotal();
		void checkFreeHeapLinear();
		void show(u32);
		void moveChildren(s32);

		void* getUnk8() const { return unk8; }

	public:
		/* 0x0 */ u8 unk0;
		/* 0x1 */ u8 unk1;
		/* 0x2 */ u16 unk2;
		/* 0x4 */ u32 unk4;
		/* 0x8 */ u8* unk8;
		/* 0xC */ u32 unkC;
		/* 0x10 */ u32 unk10;
		/* 0x14 */ THeap* unk14;
		/* 0x18 */ THeap* unk18;
		/* 0x1C */ THeap* mNext;
		/* 0x20 */ THeap* unk20;
		/* 0x24 */ THeap* unk24;
		/* 0x28 */ THeap* unk28;
	};

	class TSolidHeap {
	public:
		TSolidHeap();
		TSolidHeap(u8*, s32);
		~TSolidHeap() { }

		void init(u8*, s32);
		void* alloc(s32);
		u32 getRemain();
		void freeLast();
		void freeAll();

	public:
		/* 0x00 */ u8* mStart;
		/* 0x04 */ u8* mEnd;
		/* 0x08 */ int mSize;
		/* 0x0C */ int unkC;
		/* 0x10 */ u8* unk10;
	};

	extern u32 global_id;

	void aramFinish(u32);
	void dmaAramToAram(u32, u32, u32);
	void dmaDramToDram(u32, u32, u32);
} // namespace Kernel

} // namespace JASystem

#endif // JASHEAPCTRL_HPP
