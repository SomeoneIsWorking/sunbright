#pragma once

enum class SbSpcSwapResult {
    Swapped,
    AlreadyHostEndian,
    BadMagic,
    BadLayout,
};

// Convert an on-disc SPCB script's structural words to host byte order.
// Repeated calls are content-idempotent, including when a fresh blob reuses a
// previously freed address.
SbSpcSwapResult sb_spc_swap_to_host(unsigned char* data);
