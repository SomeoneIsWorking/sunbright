// bcsv_swap.cpp — see bcsv_swap.h.
#include "bcsv_swap.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <new>

namespace smsport::assets {
namespace {

// JMAP_VALUE_TYPE_* from ToolData.hpp (mirrored here so this module doesn't
// need to include the game header just for 8 constants).
enum JMapValueType : uint8_t {
    kLong      = 0,
    kString    = 1,
    kFloat     = 2,
    kLong2     = 3,
    kShort     = 4,
    kByte      = 5,
    kStringPtr = 6,
    kNull      = 7,
};

inline void sw16(uint8_t* p) {
    uint8_t t = p[0]; p[0] = p[1]; p[1] = t;
}
inline void sw32(uint8_t* p) {
    uint8_t t0 = p[0], t1 = p[1];
    p[0] = p[3]; p[1] = p[2];
    p[2] = t1;   p[3] = t0;
}

inline uint32_t be32_at(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
           | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint16_t be16_at(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

// Node storage MUST come from std::malloc, NOT the global operator new — same
// rationale as timg_swap.cpp/restlut_swap.cpp's MallocAlloc: this set is a
// process-global that must outlive JKR heap freeAll()s.
template <class T> struct MallocAlloc {
    using value_type = T;
    MallocAlloc() = default;
    template <class U> MallocAlloc(const MallocAlloc<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = std::malloc(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { std::free(p); }
    template <class U> bool operator==(const MallocAlloc<U>&) const noexcept { return true; }
    template <class U> bool operator!=(const MallocAlloc<U>&) const noexcept { return false; }
};

// CONTENT-VERIFIED idempotency (same policy as restlut_swap_to_host /
// restimg_swap_to_host): snapshot the swapped 0x10-byte header alongside the
// pointer so a recycled address (after a JKR heap freeAll()) that now holds a
// DIFFERENT, still-big-endian .bcr is detected as "different occupant" and
// re-swapped, rather than skipped as "already done". BCSV has no magic number
// to lean on, so the header scalars themselves are the fingerprint.
struct HeaderSnapshot {
    uint8_t bytes[0x10];
};
using SwapMap = std::unordered_map<const void*, HeaderSnapshot, std::hash<const void*>,
                                   std::equal_to<const void*>,
                                   MallocAlloc<std::pair<const void* const, HeaderSnapshot>>>;
SwapMap& swapped_map() {
    static SwapMap s;
    return s;
}

}  // namespace

BcsvSwapResult bcsv_swap_to_host(const void* bcsvData) {
    BcsvSwapResult result;
    if (!bcsvData) return result;
    uint8_t* base = static_cast<uint8_t*>(const_cast<void*>(bcsvData));

    auto [it, fresh] = swapped_map().try_emplace(bcsvData);
    if (!fresh && std::memcmp(it->second.bytes, base, sizeof(HeaderSnapshot)) == 0)
        return result;  // same swapped occupant — already host-endian

    // --- header: numEntries, numFields, dataOffset, entrySize (all BE on disc) ---
    uint32_t numEntries = be32_at(base + 0x0);
    uint32_t numFields  = be32_at(base + 0x4);
    uint32_t dataOffset = be32_at(base + 0x8);
    uint32_t entrySize  = be32_at(base + 0xC);

    sw32(base + 0x0);
    sw32(base + 0x4);
    sw32(base + 0x8);
    sw32(base + 0xC);

    // --- field table: numFields * 12 bytes at offset 0x10 ---
    // JMapItem: u32 hash, u32 mask, u16 offsData, u8 shift, u8 type.
    static constexpr uint32_t kItemSize = 12;
    uint8_t* fieldTable = base + 0x10;

    // Read types BEFORE swapping the table in place (types drive the row-data
    // swap below and are read as plain bytes, so table swap order doesn't
    // matter for them — but offsData is read BE here since we need it too).
    struct FieldInfo { uint16_t offsData; uint8_t type; };
    // Small fixed-size stack buffer would need a cap; heap-allocate via
    // malloc to avoid pulling in game heap routing for this diagnostic pass.
    FieldInfo* fields = static_cast<FieldInfo*>(std::malloc(sizeof(FieldInfo) * (numFields ? numFields : 1)));
    if (!fields) {
        result.ok = false;
        return result;
    }

    for (uint32_t i = 0; i < numFields; ++i) {
        uint8_t* item = fieldTable + i * kItemSize;
        uint16_t offsData = be16_at(item + 0x8);
        uint8_t  type     = item[0xB];

        if (type > kNull) {
            // Corrupt field table — bail out WITHOUT touching row data (the
            // header + field table swapped so far are already committed, but
            // row cells are untouched; the caller must OSPanic and never hand
            // this blob to GetValue). FAIL FAST per project hard rule.
            result.ok              = false;
            result.bad_field_index = i;
            result.bad_field_type  = type;
            std::free(fields);
            return result;
        }

        fields[i].offsData = offsData;
        fields[i].type     = type;

        sw32(item + 0x0);  // hash
        sw32(item + 0x4);  // mask
        sw16(item + 0x8);  // offsData
        // shift (u8) and type (u8) are single bytes — no swap.
    }

    // --- row data: numEntries rows of entrySize bytes at dataOffset ---
    uint8_t* rows = base + dataOffset;
    for (uint32_t e = 0; e < numEntries; ++e) {
        uint8_t* row = rows + e * entrySize;
        for (uint32_t i = 0; i < numFields; ++i) {
            uint8_t* cell = row + fields[i].offsData;
            switch (fields[i].type) {
            case kLong:
            case kFloat:
            case kLong2:
            case kStringPtr:
                sw32(cell);
                break;
            case kShort:
                sw16(cell);
                break;
            case kByte:   // single byte — no-op
            case kString: // character data — MUST NOT be swapped
            case kNull:   // no storage for this slot
                break;
            }
        }
    }

    std::free(fields);

    std::memcpy(it->second.bytes, base, sizeof(HeaderSnapshot));
    return result;
}

}  // namespace smsport::assets
