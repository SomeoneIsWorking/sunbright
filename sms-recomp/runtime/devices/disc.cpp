// disc.cpp — the mounted disc image, as raw random-access bytes.
//
// The DI device serves the guest's DVD library at the REGISTER level, so all this has to
// provide is "give me `len` bytes at absolute disc offset `off`". The game's own DVD
// library, its FST parsing and every file read then run as recompiled PPC — nothing here
// marshals a guest struct, and nothing reimplements the filesystem.
//
// nod handles the container format (ISO, RVZ, CISO, ...), which is why the project's .rvz
// works without a conversion step.

#include "disc.h"

#include <lucent/log.h>
#include <nod.h>

#include <cstdlib>
#include <string>

namespace {
NodHandle* g_disc = nullptr;
}

bool disc_open(const char* path) {
    if (g_disc) return true;

    NodDiscOptions opts{};
    NodResult r = nod_disc_open(path, &opts, &g_disc);
    if (r != NOD_RESULT_OK || !g_disc) {
        lucent::error("disc", "could not open disc image '{}' (nod result {})", path, (int)r);
        return false;
    }
    lucent::info("disc", "mounted {} ({} bytes)", path, nod_disc_size(g_disc));
    return true;
}

bool disc_is_open() { return g_disc != nullptr; }

void disc_read(u64 offset, void* out, u32 len) {
    if (!g_disc) {
        lucent::error("disc", "read of {} bytes at 0x{:x} with no disc mounted", len, offset);
        std::abort();
    }
    if (nod_seek(g_disc, (int64_t)offset, 0) < 0) {
        lucent::error("disc", "seek to 0x{:x} failed", offset);
        std::abort();
    }
    // nod_read may return short even before end-of-stream, so loop until satisfied. A short
    // read that is silently accepted would leave the tail of the buffer holding whatever was
    // there before — stale data that looks like a successful load.
    u8* p = (u8*)out;
    u32 got = 0;
    while (got < len) {
        int64_t n = nod_read(g_disc, p + got, len - got);
        if (n < 0) {
            lucent::error("disc", "read error at 0x{:x} (+{} of {})", offset, got, len);
            std::abort();
        }
        if (n == 0) {
            lucent::error("disc", "unexpected end of disc at 0x{:x} (+{} of {})", offset,
                          got, len);
            std::abort();
        }
        got += (u32)n;
    }
}

void disc_close() {
    if (g_disc) { nod_free(g_disc); g_disc = nullptr; }
}
