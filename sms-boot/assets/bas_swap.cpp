#include "bas_swap.h"

#include <sb_log.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace smsport {
namespace assets {

namespace {

inline uint16_t be16(const uint8_t* p) {
    return (uint16_t)((uint32_t)p[0] << 8 | p[1]);
}

// Read a big-endian 32-bit field and store it back in host order.
inline void swap32_at(uint8_t* p) {
    uint32_t be =
        (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
    std::memcpy(p, &be, sizeof(be)); // `be` is already host-order here
}

const size_t kEntrySize = 0x20;
const size_t kHeaderSize = 8;
// A .bas holds per-animation sound events; real files are tiny. Anything past
// this is a mis-parse, not a legitimate table — refuse loudly rather than
// allocate wildly off a byte-reversed count.
const uint16_t kMaxEntries = 4096;

std::map<const void*, std::vector<uint8_t>>& cache() {
    static std::map<const void*, std::vector<uint8_t>> c;
    return c;
}

} // namespace

const void* bas_to_host(const void* be_data) {
    if (!be_data)
        return nullptr;

    std::map<const void*, std::vector<uint8_t>>& c = cache();
    std::map<const void*, std::vector<uint8_t>>::iterator it = c.find(be_data);
    if (it != c.end())
        return it->second.empty() ? nullptr : (const void*)&it->second[0];

    const uint8_t* src = (const uint8_t*)be_data;
    const uint16_t count = be16(src);

    if (count > kMaxEntries) {
        // Fail loudly rather than hand back a silently-empty table: a count this
        // large means the buffer is not a .bas (or the load path is wrong), and
        // returning null here would just degrade into "actor has no sounds".
        sb_errorf("bas",
                  "implausible .bas entry count %u (buffer %p) -- not a .bas, or the "
                  "load path is wrong",
                  (unsigned)count, be_data);
        std::abort();
    }

    const size_t len = kHeaderSize + (size_t)count * kEntrySize;
    std::vector<uint8_t> host(src, src + len);

    // header: u16 count (the following 6 bytes are padding/flags, endian-neutral)
    uint16_t host_count = count;
    std::memcpy(&host[0], &host_count, sizeof(host_count));

    // entries: id / t0 / t1 / t2 / flags are 32-bit; the tail is bytes.
    for (size_t i = 0; i < count; ++i) {
        uint8_t* e = &host[kHeaderSize + i * kEntrySize];
        for (int f = 0; f < 5; ++f)
            swap32_at(e + f * 4);
    }

    c[be_data].swap(host);
    std::vector<uint8_t>& stored = c[be_data];
    return stored.empty() ? nullptr : (const void*)&stored[0];
}

} // namespace assets
} // namespace smsport
