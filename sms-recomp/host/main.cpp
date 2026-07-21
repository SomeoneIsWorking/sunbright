// main.cpp — host entry for the standalone recomp runtime.
//
// Loads the DOL into guest RAM and starts executing recompiled code at the DOL
// entry point. This is deliberately the smallest thing that can EXECUTE: no
// hardware routing yet, so it will run until it touches a device. Where it stops is
// the information we are after — it tells us which HW seam to route to aurora next.

#include "cpu_state.h"
#include "intrinsics.h"

#include <lucent/config.h>
#include <lucent/log.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" bool rt_mem_init();
void call_ppc(CPUState& cpu, u32 address);

namespace {

// DOL: 7 text + 11 data sections. Header is big-endian: offsets[18], addrs[18],
// sizes[18], bss addr/size, then the entry point at 0xE0.
struct Dol {
    u32 entry = 0;
    struct Sec { u32 off, addr, size; };
    std::vector<Sec> sections;
    u32 bss_addr = 0, bss_size = 0;
};

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

bool load_dol(const std::string& path, Dol& out, std::vector<u8>& bytes) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { lucent::error("dol", "cannot open {}", path); return false; }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bytes.resize((size_t)n);
    size_t got = std::fread(bytes.data(), 1, (size_t)n, f);
    std::fclose(f);
    if (got != (size_t)n || n < 0x100) { lucent::error("dol", "short read on {}", path); return false; }

    const u8* h = bytes.data();
    for (int i = 0; i < 18; i++) {
        u32 off = be32(h + 0x00 + i * 4);
        u32 addr = be32(h + 0x48 + i * 4);
        u32 size = be32(h + 0x90 + i * 4);
        if (off && size) out.sections.push_back({off, addr, size});
    }
    out.bss_addr = be32(h + 0xD8);
    out.bss_size = be32(h + 0xDC);
    out.entry    = be32(h + 0xE0);
    return true;
}

// Copy a guest range in. Writes go through the raw host pointer (not sb_w*) because
// the DOL image is already big-endian — byteswapping here would corrupt it.
void guest_write(u32 addr, const u8* src, u32 size) {
    for (u32 i = 0; i < size; i++)
        if (u8* p = sb_ram_fast(addr + i)) *p = src[i];
}

} // namespace

int main(int argc, char** argv) {
    lucent::config::set_prefix("SBR_");   // SBR_LUCENT_DEBUG=mmio,rt,poll

    std::string dol_path = argc > 1 ? argv[1] : "scratch/bin/sms.dol";

    if (!rt_mem_init()) return 1;

    Dol dol;
    std::vector<u8> bytes;
    if (!load_dol(dol_path, dol, bytes)) return 1;

    lucent::info("dol", "{}: {} sections, entry 0x{:08x}", dol_path, dol.sections.size(), dol.entry);
    for (const auto& s : dol.sections) {
        guest_write(s.addr, bytes.data() + s.off, s.size);
        lucent::debug("dol", "section -> 0x{:08x} +0x{:x}", s.addr, s.size);
    }
    if (dol.bss_size) {
        for (u32 i = 0; i < dol.bss_size; i++)
            if (u8* p = sb_ram_fast(dol.bss_addr + i)) *p = 0;
        lucent::debug("dol", "bss cleared 0x{:08x} +0x{:x}", dol.bss_addr, dol.bss_size);
    }

    CPUState cpu{};
    // The GC boots with a stack near the top of MEM1; __start sets up its own, but a
    // sane initial r1 keeps any early prologue from writing through address 0.
    cpu.gpr[1] = 0x816FFFF0u;
    cpu.pc     = dol.entry;

    lucent::info("rt", "entering recompiled code at 0x{:08x}", dol.entry);
    call_ppc(cpu, dol.entry);
    lucent::info("rt", "returned from entry (lr=0x{:08x})", cpu.lr);
    return 0;
}
