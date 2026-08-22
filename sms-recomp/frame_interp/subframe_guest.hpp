#pragma once

#include "../overrides/overrides.h"

#include <intrinsics.h>

#include <cstdio>
#include <cstring>

namespace sb::interp60 {

inline float guest_f32(u32 address) {
    const u32 bits = sb_r32(address);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

inline void guest_w_f32(u32 address, float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof bits);
    sb_w32(address, bits);
}

// Read a TNameRef's name (vptr @ 0x00, const char* mName @ 0x04).
inline void guest_name(u32 object, char* output, size_t capacity) {
    output[0] = '\0';
    if (!sb_ram_fast(object))
        return;
    const u32 name = sb_r32(object + 4);
    if (!sb_ram_fast(name)) {
        std::snprintf(output, capacity, "<unreadable>");
        return;
    }
    size_t index = 0;
    for (; index + 1 < capacity; ++index) {
        const u8 character = sb_r8(name + static_cast<u32>(index));
        if (character == 0)
            break;
        output[index] = static_cast<char>(character);
    }
    output[index] = '\0';
}

} // namespace sb::interp60
