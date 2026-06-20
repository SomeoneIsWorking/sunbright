// platform_test.cpp — TDD harness for the PC platform bring-up (platform_impl.cpp).
//
// Verifies (1) PlatformInit stands up os(arena/heap)+vi+pad so the game could run,
// and (2) the REAL PC-native disc path end-to-end: write a synthetic plain GCM file
// (boot header @0x420 -> FST -> file data), open it through sb_platform_open_gcm
// (host fopen, no Dolphin), and DVDOpen+Read a file byte-exact. No GameCube, no ROM.

#include "platform.h"
#include "dvd_disc.h"
#include <dolphin/os.h>
#include <dolphin/dvd.h>
#include <dolphin/vi.h>
#include <dolphin/pad.h>
#include "vi_present.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

static void put_be32(uint8_t* p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

// Build a minimal plain GCM: boot header at 0x420 with FSTOffset/FSTSize, an FST with
// root + one file "/hi.txt" -> "HELLO", written to `path`. Returns true on success.
static bool write_synth_gcm(const char* path) {
    const char* content = "HELLO";
    uint32_t fileOff = 0x4000;
    // FST: 2 entries (root + file) + strtab
    std::vector<uint8_t> fst(2 * 12, 0);
    // strtab: "" (root) + "hi.txt"
    std::vector<char> str;
    str.push_back('\0');           // root name @0
    uint32_t nm = (uint32_t)str.size();
    for (const char* s = "hi.txt"; *s; ++s) str.push_back(*s);
    str.push_back('\0');
    // root dir entry: type1, next=2
    fst[0] = 1; put_be32(&fst[4], 0); put_be32(&fst[8], 2);
    // file entry: type0, nameOff=nm, off=fileOff, len
    fst[12] = 0; fst[13]=(nm>>16)&0xFF; fst[14]=(nm>>8)&0xFF; fst[15]=nm&0xFF;
    put_be32(&fst[16], fileOff); put_be32(&fst[20], (uint32_t)std::strlen(content));
    fst.insert(fst.end(), str.begin(), str.end());

    uint32_t fstOff = 0x2000;
    std::vector<uint8_t> disc(0x4000 + 16, 0);
    put_be32(&disc[0x424], fstOff);
    put_be32(&disc[0x428], (uint32_t)fst.size());
    // place FST
    if (fstOff + fst.size() > disc.size()) disc.resize(fstOff + fst.size());
    std::memcpy(&disc[fstOff], fst.data(), fst.size());
    // place file data
    if (fileOff + std::strlen(content) > disc.size()) disc.resize(fileOff + std::strlen(content));
    std::memcpy(&disc[fileOff], content, std::strlen(content));

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(disc.data(), 1, disc.size(), f) == disc.size();
    std::fclose(f);
    return ok;
}

static void test_bringup() {
    char* argv[1] = { (char*)"test" };
    chk(sb::platform::PlatformInit(1, argv) == TRUE, "PlatformInit ok");
    // os: arena set + heap alloc works.
    chk(OSGetArenaLo() != nullptr && OSGetArenaHi() > OSGetArenaLo(), "arena set");
    void* p = OSAllocFromHeap(0, 4096);
    chk(p != nullptr, "heap alloc after init");
    OSFreeToHeap(0, p);
    // vi: heartbeat increments.
    sb_vi_set_pacing(false);
    u32 r0 = VIGetRetraceCount();
    VIWaitForRetrace();
    chk(VIGetRetraceCount() == r0 + 1, "vi heartbeat up after init");
    // pad: read returns (chan0 present by default).
    PADStatus st[4];
    PADRead(st);
    chk(st[0].err == PAD_ERR_NONE, "pad chan0 present after init");
}

static void test_gcm_disc() {
    const char* path = "scratch/platform_test_disc.gcm";
    if (!write_synth_gcm(path)) {
        // scratch/ may not exist from the test's CWD; try cwd-local fallback.
        path = "platform_test_disc.gcm";
        chk(write_synth_gcm(path), "write synth gcm");
    }
    chk(sb_platform_open_gcm(path) == true, "open gcm via host fopen + load FST");
    DVDFileInfo fi;
    chk(DVDOpen((char*)"/hi.txt", &fi) == TRUE, "DVDOpen on real gcm");
    chk(fi.length == 5, "hi.txt length");
    char buf[8] = {0};
    long n = DVDReadPrio(&fi, buf, (long)fi.length, 0, 2);
    chk(n == 5 && std::memcmp(buf, "HELLO", 5) == 0, "read HELLO from real gcm file");
    DVDClose(&fi);
    std::remove(path);
}

int main() {
    std::printf("== platform bring-up + PC disc reader tests ==\n");
    test_bringup();
    test_gcm_disc();
    sb::platform::PlatformShutdown();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
