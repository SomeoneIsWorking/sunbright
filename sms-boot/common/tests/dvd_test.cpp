// dvd_test.cpp — TDD harness for the DVD filesystem seam (native/platform/dvd_impl.cpp).
//
// Builds a SYNTHETIC GC disc entirely in memory (a real FST + a disc byte image)
// and verifies the shipping seam functions against spec-computed truth: path->
// entrynum (absolute, relative, "..", case-insensitive, missing), open/fast-open,
// synchronous + async file reads (byte-exact), directory iteration. GameCube-free,
// no ROM, deterministic. Each tested function IS the shipping extern "C" seam fn.

#include <dolphin/dvd.h>
#include "dvd_disc.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

// --- synthetic disc -------------------------------------------------------
// Layout we build:
//   /            (root, entry 0)
//   /readme.txt  (file, entry 1)  -> "HELLO DVD"
//   /data        (dir,  entry 2)
//   /data/x.bin  (file, entry 3)  -> bytes 0,1,2,...,63
//   /data/sub    (dir,  entry 4)
//   /data/sub/y  (file, entry 5)  -> "deep"
static std::vector<uint8_t> g_disc_bytes;

static void put_be32(uint8_t* p, uint32_t v) {
    p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}

// disc-source callback over g_disc_bytes
static bool disc_read(void* dst, u32 length, u32 offset, void*) {
    if ((size_t)offset + length > g_disc_bytes.size()) return false;
    std::memcpy(dst, g_disc_bytes.data() + offset, length);
    return true;
}

static const char* g_readme = "HELLO DVD";
static const char* g_deep = "deep";

static void build_disc() {
    const int N = 6;  // entries
    // string table
    struct { const char* name; } names[N] = {
        {""}, {"readme.txt"}, {"data"}, {"x.bin"}, {"sub"}, {"y"} };
    std::vector<char> strtab;
    uint32_t nameOff[N];
    for (int i = 0; i < N; ++i) {
        nameOff[i] = (uint32_t)strtab.size();
        for (const char* s = names[i].name; *s; ++s) strtab.push_back(*s);
        strtab.push_back('\0');
    }

    // place file data in the disc image after the FST region; pick fixed offsets.
    uint32_t off_readme = 0x8000;
    uint32_t off_xbin   = 0x9000;
    uint32_t off_y      = 0xA000;
    g_disc_bytes.assign(0xB000, 0);
    std::memcpy(&g_disc_bytes[off_readme], g_readme, std::strlen(g_readme));
    for (int i = 0; i < 64; ++i) g_disc_bytes[off_xbin + i] = (uint8_t)i;
    std::memcpy(&g_disc_bytes[off_y], g_deep, std::strlen(g_deep));

    // FST entries (12 bytes each)
    std::vector<uint8_t> fst(N * 12 + strtab.size(), 0);
    auto E = [&](int i){ return &fst[i*12]; };
    auto set_file = [&](int i, uint32_t off, uint32_t len){
        E(i)[0]=0; E(i)[1]=(nameOff[i]>>16)&0xFF; E(i)[2]=(nameOff[i]>>8)&0xFF; E(i)[3]=nameOff[i]&0xFF;
        put_be32(E(i)+4, off); put_be32(E(i)+8, len); };
    auto set_dir = [&](int i, uint32_t parent, uint32_t next){
        E(i)[0]=1; E(i)[1]=(nameOff[i]>>16)&0xFF; E(i)[2]=(nameOff[i]>>8)&0xFF; E(i)[3]=nameOff[i]&0xFF;
        put_be32(E(i)+4, parent); put_be32(E(i)+8, next); };

    set_dir (0, 0, N);                              // root: next = entry count
    set_file(1, off_readme, (uint32_t)std::strlen(g_readme));
    set_dir (2, 0, 6);                              // /data covers entries 3,4,5 -> next=6
    set_file(3, off_xbin, 64);
    set_dir (4, 2, 6);                              // /data/sub covers entry 5 -> next=6
    set_file(5, off_y, (uint32_t)std::strlen(g_deep));
    std::memcpy(&fst[N*12], strtab.data(), strtab.size());

    sb_dvd_set_disc_source(disc_read, nullptr);
    sb_dvd_set_fst(fst.data(), (u32)fst.size());
}

static void test_path() {
    chk(DVDConvertPathToEntrynum((char*)"/readme.txt") == 1, "abs readme");
    chk(DVDConvertPathToEntrynum((char*)"/data") == 2, "abs data dir");
    chk(DVDConvertPathToEntrynum((char*)"/data/x.bin") == 3, "abs nested file");
    chk(DVDConvertPathToEntrynum((char*)"/data/sub/y") == 5, "abs deep file");
    chk(DVDConvertPathToEntrynum((char*)"/DATA/X.BIN") == 3, "case-insensitive");
    chk(DVDConvertPathToEntrynum((char*)"/nope") == -1, "missing -> -1");
    chk(DVDConvertPathToEntrynum((char*)"/data/zzz") == -1, "missing nested -> -1");
    chk(DVDConvertPathToEntrynum((char*)"/") == 0, "root");
    // ".." from /data/sub -> /data
    chk(DVDConvertPathToEntrynum((char*)"/data/sub/../x.bin") == 3, "dotdot");
}

static void test_open_read() {
    DVDFileInfo fi;
    chk(DVDOpen((char*)"/readme.txt", &fi) == TRUE, "open readme");
    chk(fi.length == std::strlen(g_readme), "readme length");
    char buf[64] = {0};
    long r = DVDReadPrio(&fi, buf, (long)fi.length, 0, 2);
    chk(r == (long)fi.length, "read returns len");
    chk(std::memcmp(buf, g_readme, fi.length) == 0, "readme bytes");
    DVDClose(&fi);

    // x.bin: read a sub-range with an offset.
    chk(DVDOpen((char*)"/data/x.bin", &fi) == TRUE, "open x.bin");
    chk(fi.length == 64, "x.bin length");
    uint8_t xb[16] = {0};
    r = DVDReadPrio(&fi, xb, 16, 32, 2);  // read 16 bytes from offset 32
    chk(r == 16, "x.bin partial read len");
    bool ok = true;
    for (int i = 0; i < 16; ++i) if (xb[i] != (uint8_t)(32 + i)) ok = false;
    chk(ok, "x.bin partial bytes");
    // over-read clamps to file end.
    r = DVDReadPrio(&fi, xb, 100, 60, 2);
    chk(r == 4, "over-read clamps to 4");
    DVDClose(&fi);

    // FastOpen on a directory must fail.
    chk(DVDFastOpen(2, &fi) == FALSE, "fastopen dir -> FALSE");
    chk(DVDFastOpen(999, &fi) == FALSE, "fastopen oob -> FALSE");
}

static int g_async_result = -999;
static DVDFileInfo* g_async_fi = nullptr;
static void async_cb(s32 result, DVDFileInfo* fi) { g_async_result = result; g_async_fi = fi; }

static void test_async() {
    DVDFileInfo fi;
    DVDOpen((char*)"/data/sub/y", &fi);
    char buf[8] = {0};
    BOOL ok = DVDReadAsyncPrio(&fi, buf, (s32)fi.length, 0, async_cb, 2);
    chk(ok == TRUE, "async read ok");
    chk(g_async_result == (s32)fi.length, "async callback result");
    chk(g_async_fi == &fi, "async callback fileinfo");
    chk(std::memcmp(buf, g_deep, fi.length) == 0, "async bytes");
}

static void test_chdir_relative() {
    chk(DVDChangeDir((char*)"/data") == TRUE, "chdir /data");
    chk(DVDConvertPathToEntrynum((char*)"x.bin") == 3, "relative after chdir");
    chk(DVDConvertPathToEntrynum((char*)"sub/y") == 5, "relative nested");
    DVDChangeDir((char*)"/");  // restore
}

static void test_dir_iter() {
    DVDDir dir;
    chk(DVDOpenDir((char*)"/data", &dir) == TRUE, "opendir /data");
    DVDDirEntry de;
    int files = 0, dirs = 0;
    while (DVDReadDir(&dir, &de)) {
        if (de.isDir) ++dirs; else ++files;
    }
    DVDCloseDir(&dir);
    chk(files == 1 && dirs == 1, "/data has 1 file (x.bin) + 1 dir (sub) as direct children");
}

int main() {
    std::printf("== DVD seam unit tests ==\n");
    build_disc();
    DVDInit();
    test_path();
    test_open_read();
    test_async();
    test_chdir_relative();
    test_dir_iter();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
