// RETIRED (2026-06-12): the synthetic-token loss recovery that lived here patched the guest
// TDrawSyncManager fifo when a boundary's token message was lost. The whole guest accounting
// (message queue + counting threadFunc + guest TFifo) is now replaced by the native
// TDrawSyncManager port in sms_drawsync_lossproof.cpp — order-independent boundary/credit
// accounting with a state-derived breakpoint policy — so there is no guest fifo left to
// recover. History: the recovery's `size >= 2` guard refused the ordering-underflow terminal
// state (fifo holding one phantom entry, breakpoint enabled at a completed boundary), which
// is what made the Delfino-entry backpressure wedge permanent.
#include "../overrides.h"

bool sunbright_drawsync_recover(CPUState&) { return false; }
