// OWNED COPY of reference/sms/src/JSystem/JKernel/JKRAramPiece.cpp — keep in
// sync. 64-bit-port change ONLY: the ARAM DMA source/destination each hold a
// HOST main-RAM pointer in the native build, so prepareCommand/orderAsync/
// orderSync widen their source/destination params from u32 to uintptr_t (matching
// the JKRAramPiece.hpp shadow's widened mSrc/mDst + ARQRequest.source/dest), and
// doneDMA takes uintptr_t (it is the ARQCallback the driver invokes with the
// command address, then casts back to JKRAMCommand*; passing a u32 callback where
// ARQCallback=void(*)(uintptr_t) is required is invalid). The `(u32)command`
// callback arg becomes `(uintptr_t)command`. Verbatim otherwise.
#include <JSystem/JKernel/JKRAramPiece.hpp>
#include <JSystem/JKernel/JKRAram.hpp>
#include <JSystem/JKernel/JKRDecomp.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <dolphin/os.h>
#include <stdint.h>

JKRAMCommand* JKRAramPiece::prepareCommand(int direction, uintptr_t src,
                                           uintptr_t dst, u32 length,
                                           JKRAramBlock* block,
                                           JKRAMCommand::AsyncCallback callback)
{
	JKRAMCommand* command = new (JKRHeap::getSystemHeap(), -4) JKRAMCommand();
	command->mTransferDirection = direction;
	command->mSrc               = src;
	command->mDst               = dst;
	command->mAramBlock         = block;
	command->mDataLength        = length;
	command->mCallback          = callback;
	return command;
}

void JKRAramPiece::sendCommand(JKRAMCommand* command) { startDMA(command); }

JSUList<JKRAMCommand> JKRAramPiece::sAramPieceCommandList;
OSMutex JKRAramPiece::mMutex;

JKRAMCommand* JKRAramPiece::orderAsync(int direction, uintptr_t source,
                                       uintptr_t destination, u32 length,
                                       JKRAramBlock* block,
                                       JKRAMCommand::AsyncCallback callback)
{
	lock();
	if ((source & 0x1f) != 0 || (destination & 0x1f) != 0) {
		OSReport("direction = %x\n", direction);
		OSReport("source = %x\n", source);
		OSReport("destination = %x\n", destination);
		OSReport("length = %x\n", length);
		OSPanic(__FILE__, 102, "Abort.");
	}

	Message* message      = new (JKRHeap::getSystemHeap(), -4) Message();
	JKRAMCommand* command = JKRAramPiece::prepareCommand(
	    direction, source, destination, length, block, callback);
	message->field_0x00 = 1;
	message->command    = command;

	OSSendMessage(&JKRAram::sMessageQueue, message, OS_MESSAGE_BLOCK);
	if (command->mCallback != nullptr) {
		sAramPieceCommandList.append(&command->mPieceLink);
	}

	unlock();
	return command;
}

bool JKRAramPiece::sync(JKRAMCommand* command, int is_non_blocking)
{
	OSMessage message;

	lock();
	if (is_non_blocking == 0) {
		OSReceiveMessage(&command->mMessageQueue, &message, OS_MESSAGE_BLOCK);
		sAramPieceCommandList.remove(&command->mPieceLink);
		unlock();
		return TRUE;
	}

	BOOL result = OSReceiveMessage(&command->mMessageQueue, &message,
	                               OS_MESSAGE_NOBLOCK);
	if (!result) {
		unlock();
		return FALSE;
	}

	sAramPieceCommandList.remove(&command->mPieceLink);
	unlock();
	return TRUE;
}

bool JKRAramPiece::orderSync(int direction, uintptr_t source,
                             uintptr_t destination, u32 length,
                             JKRAramBlock* block)
{
	lock();

	JKRAMCommand* command = JKRAramPiece::orderAsync(
	    direction, source, destination, length, block, nullptr);
	bool result = JKRAramPiece::sync(command, 0);
	delete command;

	unlock();
	return result;
}

void JKRAramPiece::startDMA(JKRAMCommand* command)
{
	if (command->mTransferDirection == 1) {
		DCInvalidateRange((void*)command->mDst, command->mDataLength);
	} else {
		DCStoreRange((void*)command->mSrc, command->mDataLength);
	}

	ARQPostRequest(&command->mRequest, 0, command->mTransferDirection, 0,
	               command->mSrc, command->mDst, command->mDataLength,
	               JKRAramPiece::doneDMA);
}

void JKRAramPiece::doneDMA(uintptr_t requestAddress)
{
	JKRAMCommand* command = (JKRAMCommand*)requestAddress;

	if (command->mTransferDirection == 1) {
		DCInvalidateRange((void*)command->mDst, command->mDataLength);
	}

	if (command->field_0x60 != 0) {
		if (command->field_0x60 == 2) {
			JKRDecomp::sendCommand(command->mDecompCommand);
		}
		return;
	}

	if (command->mCallback) {
		(*command->mCallback)((uintptr_t)command);
	} else if (command->field_0x5C) {
		OSSendMessage(command->field_0x5C, command, OS_MESSAGE_NOBLOCK);
	} else {
		OSSendMessage(&command->mMessageQueue, command, OS_MESSAGE_NOBLOCK);
	}
}

JKRAMCommand::JKRAMCommand()
    : mPieceLink(this)
    , field_0x30(this)
{
	OSInitMessageQueue(&mMessageQueue, &mMessage, OS_MESSAGE_BLOCK);
	mCallback  = nullptr;
	field_0x5C = nullptr;
	field_0x60 = 0;
	field_0x8C = nullptr;
	field_0x90 = nullptr;
	field_0x94 = nullptr;
}

JKRAMCommand::~JKRAMCommand()
{
	if (field_0x8C) {
		delete field_0x8C;
	}
	if (field_0x90) {
		delete field_0x90;
	}

	if (field_0x94) {
		JKRHeap::free(field_0x94, nullptr);
	}
}
