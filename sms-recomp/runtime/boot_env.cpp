// boot_env.cpp — the low-memory state a real GameCube boot leaves behind.
//
// On hardware the IPL and the disc's apploader run before the game's DOL: they copy the
// disc ID into low memory, load the filesystem table, publish the arena bounds, and record
// the clock speeds. The game's OS and DVD libraries read all of it as a given.
//
// Loading only the DOL leaves every one of those words zero, which does not fail loudly —
// it fails subtly, far away. The clock speed at 0x800000F8 is the sharp example: the DVD
// library derives command timeouts from it, and a zero bus clock makes every timeout expire
// instantly, so the command queue retries forever instead of making progress.

#include "boot_env.h"

#include "disc.h"
#include "intrinsics.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// Low-memory globals (OS_BASE_CACHED). Names from the SDK's OSGlobals.
constexpr u32 OS_DISC_ID       = 0x80000000;   // 0x20 bytes, copied from the disc header
constexpr u32 OS_BOOT_MAGIC    = 0x80000020;
constexpr u32 OS_BOOT_VERSION  = 0x80000024;
constexpr u32 OS_PHYSICAL_SIZE = 0x80000028;
constexpr u32 OS_CONSOLE_TYPE  = 0x8000002C;
constexpr u32 OS_ARENA_LO      = 0x80000030;
constexpr u32 OS_ARENA_HI      = 0x80000034;
constexpr u32 OS_FST_LOCATION  = 0x80000038;
constexpr u32 OS_FST_MAX_SIZE  = 0x8000003C;
constexpr u32 OS_BUS_CLOCK     = 0x800000F8;
constexpr u32 OS_CPU_CLOCK     = 0x800000FC;

constexpr u32 BOOT_MAGIC   = 0x0D15EA5E;   // what the IPL writes to say "a game booted"
constexpr u32 BOOT_VERSION = 1;
constexpr u32 MEM1_SIZE    = 0x01800000;   // 24 MB
constexpr u32 CONSOLE_TYPE = 0x00000003;   // retail

// The real clocks. These are not decoration: the OS derives its timebase and every
// timeout from them.
constexpr u32 BUS_CLOCK = 162000000;
constexpr u32 CPU_CLOCK = 486000000;

// Disc-header offsets.
constexpr u32 H_FST_OFFSET   = 0x424;
constexpr u32 H_FST_SIZE     = 0x428;
constexpr u32 H_FST_MAX_SIZE = 0x42C;

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

} // namespace

bool boot_env_setup(u32 arena_lo) {
    if (!disc_is_open()) {
        lucent::error("boot", "no disc mounted — cannot build the boot environment");
        return false;
    }

    u8 header[0x440];
    disc_read(0, header, sizeof(header));

    // Disc ID: game code, maker, disc number, version, and the 0xC2339F3D magic.
    for (u32 i = 0; i < 0x20; i++) sb_w8(OS_DISC_ID + i, header[i]);

    const u32 fst_off = be32(header + H_FST_OFFSET);
    const u32 fst_len = be32(header + H_FST_SIZE);
    u32 fst_max       = be32(header + H_FST_MAX_SIZE);
    if (fst_max < fst_len) fst_max = fst_len;

    if (fst_len == 0 || fst_len > MEM1_SIZE) {
        lucent::error("boot", "disc header reports an implausible FST size 0x{:x}", fst_len);
        return false;
    }

    // The apploader parks the FST at the top of MEM1 and hands everything below it to the
    // game as the arena. Align down so the game's own allocator sees a sane boundary.
    const u32 fst_addr = (0x80000000u + MEM1_SIZE - fst_max) & ~0x1Fu;

    std::vector<u8> fst(fst_len);
    disc_read(fst_off, fst.data(), fst_len);
    for (u32 i = 0; i < fst_len; i++) sb_w8(fst_addr + i, fst[i]);

    sb_w32(OS_BOOT_MAGIC,    BOOT_MAGIC);
    sb_w32(OS_BOOT_VERSION,  BOOT_VERSION);
    sb_w32(OS_PHYSICAL_SIZE, MEM1_SIZE);
    sb_w32(OS_CONSOLE_TYPE,  CONSOLE_TYPE);
    sb_w32(OS_FST_LOCATION,  fst_addr);
    sb_w32(OS_FST_MAX_SIZE,  fst_max);
    sb_w32(OS_ARENA_HI,      fst_addr);
    sb_w32(OS_BUS_CLOCK,     BUS_CLOCK);
    sb_w32(OS_CPU_CLOCK,     CPU_CLOCK);
    // Everything from the end of the DOL up to the FST is the game's heap.
    sb_w32(OS_ARENA_LO,      (arena_lo + 0x1F) & ~0x1Fu);

    lucent::info("boot", "FST 0x{:x} bytes -> 0x{:08x}; arena 0x{:08x}..0x{:08x}; bus {} MHz",
                 fst_len, fst_addr, sb_r32(OS_ARENA_LO), fst_addr, BUS_CLOCK / 1000000);
    return true;
}

u32 boot_env_fst_addr() { return sb_r32(OS_FST_LOCATION); }
