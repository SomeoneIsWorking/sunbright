// gx_parse.h — Dolphin-free SHADOW for the native build.
//
// runtime/ngx/ngx_decode.h includes "gx_parse.h" only for the integer typedefs and
// the GxFrameInfo result struct. The real runtime/gx_parse.h pulls in cpu_state.h +
// Dolphin's OpcodeDecoder (the Dolphin-based frame analyzer), which we don't want in
// the standalone native renderer. This shadow provides just what ngx_decode needs, so
// the pure mesh/vertex/decode code compiles GameCube/Dolphin-free. Placed first on the
// sms-render include path (mirrors native/shim's MSL shadowing).
#pragma once
#include <cstdint>
#include <vector>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

// Result struct ngx_decode fills (same shape as the real gx_parse.h GxFrameInfo).
struct GxFrameInfo {
    std::vector<u32> token_offsets;
    struct ArrayPatch { u32 offset; u32 array; u32 base; };
    std::vector<ArrayPatch> mtx_arrays;
    u32 prims = 0, display_lists = 0, copies = 0;
    bool ok = false;
    u32 fail_offset = 0;
    u8  fail_opcode = 0;
    u32 total = 0;
};
