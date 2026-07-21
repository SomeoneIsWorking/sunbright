#pragma once
#include <cstdint>
#include <vector>
#include <string>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

// DOL executable format (GameCube/Wii main executable).
// Big-endian on disk. We byte-swap on load.

struct DOLHeader {
    u32 text_offset[7];   // File offset of text sections
    u32 data_offset[11];  // File offset of data sections
    u32 text_addr[7];     // Load address of text sections
    u32 data_addr[11];    // Load address of data sections
    u32 text_size[7];     // Size of text sections
    u32 data_size[11];    // Size of data sections
    u32 bss_addr;         // BSS segment address
    u32 bss_size;         // BSS segment size
    u32 entry_point;      // Start address (GC vaddr, e.g. 0x80003100)
    u8  pad[28];
};
static_assert(sizeof(DOLHeader) == 0x100, "DOLHeader must be 256 bytes");

struct DOLSection {
    u32    addr;      // Load address
    u32    size;      // Byte count
    std::vector<u8> data;
    bool   is_text;
};

struct DOL {
    DOLHeader              hdr;
    std::vector<DOLSection> sections;
    u32                    entry;

    // Load from raw bytes (big-endian, as read from disc)
    static DOL from_bytes(const std::vector<u8>& bytes);

    // Find section containing a given address
    const DOLSection* section_at(u32 addr) const;

    // Read a u32 from the DOL address space (returns 0 if not mapped)
    u32 read_u32(u32 addr) const;

    // All text sections concatenated with their addresses
    std::vector<std::pair<u32, std::vector<u8>>> text_sections() const;
};
