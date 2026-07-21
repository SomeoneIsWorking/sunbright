#include "dol_parser.h"
#include <cstring>
#include <stdexcept>

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

DOL DOL::from_bytes(const std::vector<u8>& bytes) {
    if (bytes.size() < sizeof(DOLHeader))
        throw std::runtime_error("DOL too small");

    DOL dol;
    const u8* b = bytes.data();

    // Parse header (big-endian)
    for (int i = 0; i < 7;  i++) dol.hdr.text_offset[i] = be32(b + 0x00 + i*4);
    for (int i = 0; i < 11; i++) dol.hdr.data_offset[i] = be32(b + 0x1C + i*4);
    for (int i = 0; i < 7;  i++) dol.hdr.text_addr[i]   = be32(b + 0x48 + i*4);
    for (int i = 0; i < 11; i++) dol.hdr.data_addr[i]   = be32(b + 0x64 + i*4);
    for (int i = 0; i < 7;  i++) dol.hdr.text_size[i]   = be32(b + 0x90 + i*4);
    for (int i = 0; i < 11; i++) dol.hdr.data_size[i]   = be32(b + 0xAC + i*4);
    dol.hdr.bss_addr    = be32(b + 0xD8);
    dol.hdr.bss_size    = be32(b + 0xDC);
    dol.hdr.entry_point = be32(b + 0xE0);
    dol.entry           = dol.hdr.entry_point;

    // Load text sections
    for (int i = 0; i < 7; i++) {
        if (dol.hdr.text_size[i] == 0) continue;
        DOLSection s;
        s.addr    = dol.hdr.text_addr[i];
        s.size    = dol.hdr.text_size[i];
        s.is_text = true;
        u32 off   = dol.hdr.text_offset[i];
        if (off + s.size > bytes.size())
            throw std::runtime_error("DOL text section out of bounds");
        s.data.assign(b + off, b + off + s.size);
        dol.sections.push_back(std::move(s));
    }

    // Load data sections
    for (int i = 0; i < 11; i++) {
        if (dol.hdr.data_size[i] == 0) continue;
        DOLSection s;
        s.addr    = dol.hdr.data_addr[i];
        s.size    = dol.hdr.data_size[i];
        s.is_text = false;
        u32 off   = dol.hdr.data_offset[i];
        if (off + s.size > bytes.size())
            throw std::runtime_error("DOL data section out of bounds");
        s.data.assign(b + off, b + off + s.size);
        dol.sections.push_back(std::move(s));
    }

    return dol;
}

const DOLSection* DOL::section_at(u32 addr) const {
    for (const auto& s : sections) {
        if (addr >= s.addr && addr < s.addr + s.size)
            return &s;
    }
    return nullptr;
}

u32 DOL::read_u32(u32 addr) const {
    const DOLSection* s = section_at(addr);
    if (!s) return 0;
    u32 off = addr - s->addr;
    if (off + 4 > s->data.size()) return 0;
    return be32(s->data.data() + off);
}

std::vector<std::pair<u32, std::vector<u8>>> DOL::text_sections() const {
    std::vector<std::pair<u32, std::vector<u8>>> result;
    for (const auto& s : sections) {
        if (s.is_text)
            result.emplace_back(s.addr, s.data);
    }
    return result;
}
