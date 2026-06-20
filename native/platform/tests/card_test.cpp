// card_test.cpp — TDD harness for the CARD seam (native/platform/card_impl.cpp).
//
// Builds a real card image in scratch/memcard_test_chanN.raw, exercises the
// 14 extern "C" functions, and verifies correct behaviour against spec-computed
// ground truth. No ROM, no Dolphin, no guest RAM — pure host POSIX I/O.
//
// Tests:
//  1. probe/mount on a blank image → CARD_RESULT_BROKEN (game will format it).
//  2. format → subsequent probe/mount reports CARD_RESULT_READY.
//  3. create + open a file; check CARDFileInfo fields.
//  4. write a pattern, close, re-open, read back — byte-exact roundtrip.
//  5. free-blocks accounting: before create vs after.
//  6. GetStatus / SetStatus roundtrip.
//  7. close + open again (persistence check).
//  8. create duplicate file → CARD_RESULT_EXIST.

#include <dolphin/card.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>

// Override the image path so tests use a dedicated scratch file that does not
// collide with the real memcard_chan0.raw. We do this by pointing the test
// to a temp path via the CARD_TEST_IMG env var — but card_impl.cpp hardcodes
// the path. Instead we simply remove the existing image before the test run
// so we always get a fresh blank, and run tests against chan=1 (an unlikely
// to be used slot in production runs).
// The test creates scratch/memcard_chan1.raw.

static int g_fail = 0, g_checks = 0;

static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}
static void chki(long got, long want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_fail;
        std::printf("  FAIL: %s got=%ld want=%ld\n", what, got, want);
    }
}

// Use chan=1 to avoid colliding with a user's chan=0 card image.
static const s32 CHAN = 1;

// Wipe the card image (if it exists) so every test run starts fresh.
static void reset_image() {
    // Remove by unlinking. unlink is safe even if the file doesn't exist.
    (void)unlink("scratch/memcard_chan1.raw");
}

// ---------------------------------------------------------------------------
// test_probe_blank — probe + mount a blank (never-formatted) image.
// After creating the raw file, mount should report BROKEN (blank).
static void test_probe_blank() {
    CARDInit();
    reset_image();

    s32 memSz = 0, secSz = 0;
    s32 r = CARDProbeEx(CHAN, &memSz, &secSz);
    chki(r, CARD_RESULT_READY, "probe blank: READY (image created)");
    chk(memSz > 0, "probe blank: memSize > 0");
    chki(secSz, 8192, "probe blank: sectorSize = 8192");

    r = CARDMount(CHAN, nullptr, nullptr);
    chki(r, CARD_RESULT_BROKEN, "mount blank: BROKEN (unformatted)");

    // Unmount so subsequent tests can re-mount.
    CARDUnmount(CHAN);
}

// test_format — format a blank card; subsequent mount must succeed.
static void test_format() {
    CARDInit();
    reset_image();

    // Ensure the image exists (probe creates it).
    CARDProbeEx(CHAN, nullptr, nullptr);

    long r = CARDFormat((long)CHAN);
    chki(r, CARD_RESULT_READY, "format: READY");

    // Format leaves the card mounted; verify.
    s32 r2 = CARDCheck((long)CHAN);
    chki(r2, CARD_RESULT_READY, "check after format: READY");

    CARDUnmount(CHAN);

    // Now re-mount fresh and verify.
    r2 = CARDMount(CHAN, nullptr, nullptr);
    chki(r2, CARD_RESULT_READY, "mount after format: READY");

    s32 bytesFree = 0, filesFree = 0;
    r2 = CARDFreeBlocks(CHAN, &bytesFree, &filesFree);
    chki(r2, CARD_RESULT_READY, "freeblocks after format: READY");
    chk(bytesFree > 0, "freeblocks: some space free");
    chki(filesFree, 127, "freeblocks: all 127 file slots free");

    CARDUnmount(CHAN);
}

// test_create_open — create a file, verify CARDFileInfo fields.
static void test_create_open() {
    CARDInit();
    reset_image();
    CARDProbeEx(CHAN, nullptr, nullptr);
    CARDFormat((long)CHAN);   // leaves mounted

    CARDFileInfo fi;
    long r = CARDCreate((long)CHAN, (char*)"TestFile", 8192, &fi);
    chki(r, CARD_RESULT_READY, "create: READY");
    chki(fi.chan, (long)CHAN, "create fi.chan");
    chk(fi.fileNo >= 0 && fi.fileNo < 127, "create fi.fileNo in range");
    chki(fi.length, 8192, "create fi.length = 8192 (1 block)");
    chk(fi.iBlock >= 5, "create fi.iBlock >= CDATABLK");

    CARDClose(&fi);

    // Duplicate create must fail with EXIST.
    CARDFileInfo fi2;
    r = CARDCreate((long)CHAN, (char*)"TestFile", 8192, &fi2);
    chki(r, CARD_RESULT_EXIST, "create duplicate: EXIST");

    // Open the already-created file.
    CARDFileInfo fi3;
    s32 r2 = CARDOpen(CHAN, (char*)"TestFile", &fi3);
    chki(r2, CARD_RESULT_READY, "open existing: READY");
    chki(fi3.length, 8192, "open fi.length matches");
    chki((long)fi3.iBlock, (long)fi.iBlock, "open fi.iBlock matches create");
    CARDClose(&fi3);

    // Open a non-existent file.
    r2 = CARDOpen(CHAN, (char*)"NoSuchFile", &fi3);
    chki(r2, CARD_RESULT_NOFILE, "open missing: NOFILE");

    CARDUnmount(CHAN);
}

// test_write_read — write a known pattern, re-open, read back byte-exact.
static void test_write_read() {
    CARDInit();
    reset_image();
    CARDProbeEx(CHAN, nullptr, nullptr);
    CARDFormat((long)CHAN);

    CARDFileInfo fi;
    CARDCreate((long)CHAN, (char*)"SaveFile", 8192, &fi);

    // Fill with a known pattern: byte = (index & 0xFF).
    static unsigned char wbuf[8192];
    for (int i = 0; i < 8192; ++i) wbuf[i] = (unsigned char)(i & 0xFF);

    long r = CARDWrite(&fi, wbuf, 8192, 0);
    chki(r, CARD_RESULT_READY, "write 8192 bytes: READY");
    CARDClose(&fi);

    // Re-open and read back.
    CARDFileInfo fi2;
    s32 r2 = CARDOpen(CHAN, (char*)"SaveFile", &fi2);
    chki(r2, CARD_RESULT_READY, "re-open for read: READY");

    static unsigned char rbuf[8192];
    memset(rbuf, 0xCC, sizeof rbuf);
    long r3 = CARDRead(&fi2, rbuf, 8192, 0);
    chki(r3, CARD_RESULT_READY, "read 8192 bytes: READY");

    bool match = true;
    for (int i = 0; i < 8192; ++i) {
        if (rbuf[i] != wbuf[i]) { match = false; break; }
    }
    chk(match, "read data matches written pattern (byte-exact)");
    CARDClose(&fi2);

    // Partial read at an offset.
    CARDOpen(CHAN, (char*)"SaveFile", &fi2);
    static unsigned char pbuf[256];
    memset(pbuf, 0, sizeof pbuf);
    r3 = CARDRead(&fi2, pbuf, 256, 512);
    chki(r3, CARD_RESULT_READY, "partial read at offset 512: READY");
    bool pmatch = true;
    for (int i = 0; i < 256; ++i) {
        if (pbuf[i] != (unsigned char)((512 + i) & 0xFF)) { pmatch = false; break; }
    }
    chk(pmatch, "partial read data correct");
    CARDClose(&fi2);

    CARDUnmount(CHAN);
}

// test_free_blocks_accounting — verify block counts change correctly.
static void test_free_blocks_accounting() {
    CARDInit();
    reset_image();
    CARDProbeEx(CHAN, nullptr, nullptr);
    CARDFormat((long)CHAN);

    s32 bytesBefore = 0, filesBefore = 0;
    CARDFreeBlocks(CHAN, &bytesBefore, &filesBefore);
    chk(bytesBefore > 0, "space available before create");
    chki(filesBefore, 127, "127 file slots before create");

    // Create a 3-sector file.
    CARDFileInfo fi;
    CARDCreate((long)CHAN, (char*)"BigFile", 3 * 8192, &fi);
    chki(fi.length, 3 * 8192, "create 3-block file: length = 3×8192");

    s32 bytesAfter = 0, filesAfter = 0;
    CARDFreeBlocks(CHAN, &bytesAfter, &filesAfter);
    chki(bytesBefore - bytesAfter, 3 * 8192, "3 blocks consumed");
    chki(filesBefore - filesAfter, 1, "1 file slot consumed");
    CARDClose(&fi);

    CARDUnmount(CHAN);
}

// test_getstatus_setstatus — roundtrip metadata through GetStatus/SetStatus.
static void test_getstatus_setstatus() {
    CARDInit();
    reset_image();
    CARDProbeEx(CHAN, nullptr, nullptr);
    CARDFormat((long)CHAN);

    CARDFileInfo fi;
    CARDCreate((long)CHAN, (char*)"MetaFile", 8192, &fi);
    s32 fileNo = fi.fileNo;
    CARDClose(&fi);

    // SetStatus: write gameName + company + time.
    CARDStat st;
    memset(&st, 0, sizeof st);
    strncpy(st.fileName, "MetaFile", 32);
    memcpy(st.gameName, "GMSJ", 4);
    memcpy(st.company,  "01", 2);
    st.time = 0x12345678u;
    st.iconAddr = 0xFFFFFFFFu;
    st.commentAddr = 0xFFFFFFFFu;
    long r = CARDSetStatus((long)CHAN, (long)fileNo, &st);
    chki(r, CARD_RESULT_READY, "SetStatus: READY");

    // GetStatus should return the same values.
    CARDStat st2;
    memset(&st2, 0xFF, sizeof st2);
    s32 r2 = CARDGetStatus(CHAN, fileNo, &st2);
    chki(r2, CARD_RESULT_READY, "GetStatus: READY");
    chk(memcmp(st2.gameName, "GMSJ", 4) == 0, "GetStatus gameName matches");
    chk(memcmp(st2.company, "01", 2) == 0, "GetStatus company matches");
    chk(st2.time == 0x12345678u, "GetStatus time matches");
    chki((long)st2.length, 8192, "GetStatus length = 8192");

    CARDUnmount(CHAN);
}

// test_persistence — write data, unmount, re-mount, verify data still present.
static void test_persistence() {
    CARDInit();
    reset_image();
    CARDProbeEx(CHAN, nullptr, nullptr);
    CARDFormat((long)CHAN);

    CARDFileInfo fi;
    CARDCreate((long)CHAN, (char*)"Persist", 8192, &fi);
    static unsigned char wd[8192];
    for (int i = 0; i < 8192; ++i) wd[i] = (unsigned char)(255 - (i & 0xFF));
    CARDWrite(&fi, wd, 8192, 0);
    CARDClose(&fi);
    CARDUnmount(CHAN);

    // Re-init + re-mount (simulates a new run).
    CARDInit();
    s32 r = CARDMount(CHAN, nullptr, nullptr);
    chki(r, CARD_RESULT_READY, "persistence: re-mount READY");

    CARDFileInfo fi2;
    r = CARDOpen(CHAN, (char*)"Persist", &fi2);
    chki(r, CARD_RESULT_READY, "persistence: re-open READY");

    static unsigned char rd[8192];
    memset(rd, 0, sizeof rd);
    CARDRead(&fi2, rd, 8192, 0);
    bool match = (memcmp(rd, wd, 8192) == 0);
    chk(match, "persistence: data survives unmount+remount");
    CARDClose(&fi2);

    CARDUnmount(CHAN);
}

int main() {
    std::printf("== CARD seam unit tests ==\n");
    // Ensure scratch/ exists (card_impl.cpp creates it, but just in case).
    (void)mkdir("scratch", 0755);

    test_probe_blank();
    test_format();
    test_create_open();
    test_write_read();
    test_free_blocks_accounting();
    test_getstatus_setstatus();
    test_persistence();

    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
