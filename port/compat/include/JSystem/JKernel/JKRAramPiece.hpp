// SHADOW HEADER (64-bit port) of
// reference/sms/include/JSystem/JKernel/JKRAramPiece.hpp — keep in sync.
// 64-bit-port change: the ARAM DMA "source"/"destination" pair can each hold a
// HOST main-RAM pointer (RAM->ARAM uses source=host ptr; ARAM->RAM uses
// destination=host ptr — see JKRAram::JKRAramPcs callers). On GC both were u32;
// on LP64 a host pointer truncates. Widened mSrc/mDst + the source/destination
// params of prepareCommand/orderAsync/orderSync/JKRAramPcs to uintptr_t (ARAM
// addresses also fit), and AsyncCallback's arg (callers pass `(uintptr_t)command`).
// `mTransferDirection`/`mDataLength` stay as declared. `doneDMA`'s arg is the ARQ
// request/command address — it is the ARQCallback the driver invokes (and the
// JKRAramPiece.cpp body casts it back to JKRAMCommand*), so it is widened to
// uintptr_t too (a u32 doneDMA cannot be passed where ARQCallback=void(*)(uintptr_t)
// is required). Verbatim otherwise.
#ifndef JKR_ARAM_PIECE_HPP
#define JKR_ARAM_PIECE_HPP

#include <JSystem/JSupport/JSUList.hpp>
#include <dolphin/ar.h>
#include <dolphin/os/OSMessage.h>
#include <dolphin/os/OSMutex.h>
#include <stdint.h>

class JKRAramBlock;
class JKRDecompCommand;
class JKRAMCommand {
public:
	typedef void (*AsyncCallback)(uintptr_t);

	JKRAMCommand();
	~JKRAMCommand();

public:
	/* 0x00 */ ARQRequest mRequest;
	/* 0x20 */ JSULink<JKRAMCommand> mPieceLink;
	/* 0x30 */ JSULink<JKRAMCommand> field_0x30;

	/* 0x40 */ s32 mTransferDirection;
	/* 0x44 */ u32 mDataLength;
	/* 0x48 */ uintptr_t mSrc;
	/* 0x4C */ uintptr_t mDst;
	/* 0x50 */ JKRAramBlock* mAramBlock;
	/* 0x54 */ u32 field_0x54;
	/* 0x58 */ AsyncCallback mCallback;
	/* 0x5C */ OSMessageQueue* field_0x5C;
	/* 0x60 */ s32 field_0x60;
	/* 0x64 */ JKRDecompCommand* mDecompCommand;
	/* 0x68 */ OSMessageQueue mMessageQueue;
	/* 0x88 */ OSMessage mMessage;
	/* 0x8C */ void* field_0x8C;
	/* 0x90 */ void* field_0x90;
	/* 0x94 */ void* field_0x94;
};

class JKRAramPiece {
public:
	static OSMutex mMutex;
	// TODO: fix type
	static JSUList<JKRAMCommand> sAramPieceCommandList;

public:
	struct Message {
		s32 field_0x00;
		JKRAMCommand* command;
	};

public:
	static JKRAMCommand* prepareCommand(int, uintptr_t, uintptr_t, u32,
	                                    JKRAramBlock*, JKRAMCommand::AsyncCallback);
	static void sendCommand(JKRAMCommand*);

	static JKRAMCommand* orderAsync(int, uintptr_t, uintptr_t, u32,
	                                JKRAramBlock*, JKRAMCommand::AsyncCallback);
	static bool sync(JKRAMCommand*, int);
	static bool orderSync(int, uintptr_t, uintptr_t, u32, JKRAramBlock*);
	static void startDMA(JKRAMCommand*);
	static void doneDMA(uintptr_t);

private:
	static void lock() { OSLockMutex(&mMutex); }
	static void unlock() { OSUnlockMutex(&mMutex); }
};

inline BOOL JKRAramPcs(int direction, uintptr_t source, uintptr_t destination,
                       u32 length, JKRAramBlock* block)
{
	return JKRAramPiece::orderSync(direction, source, destination, length,
	                               block);
}

#endif
