// =============================================================================
// Big-endian -> host byteswap for Koga::ToolData BCSV/JMap blobs (.bcr movie
// rumble tables, and any other future Koga::ToolData::Attach consumer).
//
// Layout (decomp/sms/include/MarioUtil/ToolData.hpp — Koga::JMapData /
// Koga::JMapItem), all on-disc fields big-endian:
//   header (0x10 bytes): s32 numEntries, s32 numFields, s32 dataOffset,
//                        u32 entrySize
//   field table (numFields * 12 bytes), starting at offset 0x10:
//     u32 hash, u32 mask, u16 offsData, u8 shift, u8 type
//   row data: numEntries rows of entrySize bytes, starting at dataOffset
//     (relative to the JMapData* base, i.e. the start of the whole blob).
//
// Koga::ToolData::Attach stores the raw blob pointer as `mData` and reads
// straight through it for the lifetime of the object (GetValue does
// `*reinterpret_cast<const s32*>(valuePtr)` / raw `const char*` — no separate
// parsed copy is ever made). That matches restlut_swap.cpp's/timg_swap.cpp's
// "swap the header (and here, the field table + row cells) in place" policy,
// NOT bmd_swap.cpp's "copy to a host-endian buffer" policy — there is no
// second buffer to copy into; `mData` IS the long-lived structure.
//
// Per-cell width comes from the field table's `type` byte (JMAP_VALUE_TYPE_*
// in ToolData.hpp), which is a closed, self-describing set:
//   0 LONG        s32   4 bytes  -> swap
//   1 STRING      bytes N/A      -> NEVER swap (character data)
//   2 FLOAT       f32   4 bytes  -> swap
//   3 LONG_2      s32   4 bytes  -> swap (alternate long encoding; still 4B)
//   4 SHORT       s16   2 bytes  -> swap
//   5 BYTE        s8/u8 1 byte   -> no-op (single byte)
//   6 STRING_PTR  u32   4 bytes  -> swap (it's an offset/index, not text)
//   7 NULL        -     0 bytes  -> no-op (no storage for this slot)
// Any type byte outside 0-7 is a corrupt/unknown field table and FAIL-FASTs
// via OSPanic (never silently skipped) per project hard rule.
//
// Idempotency: BCSV has no magic number, so — mirroring restlut_swap.cpp's
// content-verified policy — a per-pointer map records the swapped header
// bytes. A recycled address whose content no longer matches (a JKR heap
// freeAll() handed the same address to a different, still-BE .bcr) is
// re-swapped; a re-Attach of the SAME already-swapped blob is a no-op.
// =============================================================================
#pragma once
#include <cstdint>

namespace smsport::assets {

struct BcsvSwapResult {
    bool        ok    = true;   // false only on a corrupt field-table type byte
    uint32_t    bad_field_index = 0;
    uint8_t     bad_field_type  = 0;
};

// Swap a BE Koga::JMapData blob to host endianness in place (no-op if null).
// The caller (compiled with access to dolphin/os.h, unlike this BE-asset
// library) must OSPanic on !ok — a field-table type byte outside 0-7 is a
// corrupt BCSV, not a case to silently skip (FAIL FAST).
BcsvSwapResult bcsv_swap_to_host(const void* bcsvData);

}  // namespace smsport::assets
