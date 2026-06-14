// SHADOW HEADER (64-bit port) of reference/sms/include/dolphin/ar.h — keep in
// sync. (This is the ARAM/ARQ DMA interface, NOT dolphin/os*·vi*·hw_regs*, so it
// is shadowable.) 64-bit-port change: an ARQ DMA's `source`/`dest` each hold a
// HOST main-RAM pointer in the native build (MRAM<->ARAM transfers pass a host
// buffer address on one side), and the ARQCallback arg is literally a
// "pointerToARQRequest". Widened ARQRequest.source/dest, the ARQPostRequest
// source/dest params, ARStartDMA's mainmem_addr, and the ARQCallback arg from
// u32 to uintptr_t. The ARAM-side addresses also fit. ARAlloc/ARFree/ARGetSize
// (ARAM-space offsets) and owner/type/priority/length stay u32. PAL ITEMS: the
// arq.c / ar.c definitions (the ARQ/ARAM hardware driver — not yet ported) must
// match these widened signatures. Verbatim otherwise.
#ifndef _DOLPHIN_AR_H_
#define _DOLPHIN_AR_H_

#include <dolphin/types.h>
#include <stdint.h>

typedef void (*ARQCallback)(uintptr_t pointerToARQRequest);

struct ARQRequest {
	/* 0x00 */ struct ARQRequest* next;
	/* 0x04 */ u32 owner;
	/* 0x08 */ u32 type;
	/* 0x0C */ u32 priority;
	/* 0x10 */ uintptr_t source;
	/* 0x14 */ uintptr_t dest;
	/* 0x18 */ u32 length;
	/* 0x1C */ ARQCallback callback;
};

#define ARQ_DMA_ALIGNMENT 32

#define ARAM_DIR_MRAM_TO_ARAM 0x00
#define ARAM_DIR_ARAM_TO_MRAM 0x01

#define ARStartDMARead(mmem, aram, len)                                        \
	ARStartDMA(ARAM_DIR_ARAM_TO_MRAM, mmem, aram, len)
#define ARStartDMAWrite(mmem, aram, len)                                       \
	ARStartDMA(ARAM_DIR_MRAM_TO_ARAM, mmem, aram, len)

typedef struct ARQRequest ARQRequest;

#define ARQ_TYPE_MRAM_TO_ARAM ARAM_DIR_MRAM_TO_ARAM
#define ARQ_TYPE_ARAM_TO_MRAM ARAM_DIR_ARAM_TO_MRAM

#define ARQ_PRIORITY_LOW  0
#define ARQ_PRIORITY_HIGH 1

#ifdef __cplusplus
extern "C" {
#endif

// ar.c
ARQCallback ARRegisterDMACallback(ARQCallback callback);
u32 ARGetDMAStatus(void);
void ARStartDMA(u32 type, uintptr_t mainmem_addr, u32 aram_addr, u32 length);
u32 ARAlloc(u32 length);
u32 ARFree(u32* length);
int ARCheckInit(void);
u32 ARInit(u32* stack_index_addr, u32 num_entries);
void ARReset(void);
void ARSetSize(void);
u32 ARGetBaseAddress(void);
u32 ARGetSize(void);

// arq.c
void ARQInit(void);
void ARQReset(void);
void ARQPostRequest(struct ARQRequest* request, u32 owner, u32 type,
                    u32 priority, uintptr_t source, uintptr_t dest, u32 length,
                    ARQCallback callback);
void ARQRemoveRequest(struct ARQRequest* request);
void ARQRemoveOwnerRequest(u32 owner);
void ARQFlushQueue(void);
void ARQSetChunkSize(u32 size);
u32 ARQGetChunkSize(void);

#ifdef __cplusplus
}
#endif

#endif
