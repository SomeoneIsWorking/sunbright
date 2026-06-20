// card_impl.cpp — native PC implementation of the GC CARD (memory-card) seam.
//
// WHY: the native engine (libsms-native) links against the GC CARD SDK symbols
// (CARDInit, CARDMount, CARDOpen, CARDRead, CARDWrite, ...). This is their PC-native
// replacement — no Dolphin, no EXI, no DSP unlock, no CoreTiming events.
//
// DESIGN: synchronous file-backed reimplementation of the GC CARD block layer +
// directory FS. All 14 extern "C" functions the game links are defined here. The
// card is backed by scratch/memcard_chan{N}.raw (created blank on first use).
// A blank card returns CARD_RESULT_BROKEN from CARDMount so the game's own
// CARDFormat call reformats it — identical flow to inserting a new physical card.
//
// GC CARD FORMAT (standard, from card.h and native_card.cpp prior art):
//   Sector = CARD_SYSTEM_BLOCK_SIZE = 8 KB.
//   Default image: 64 sectors (5 system + 59 user) = 524288 bytes.
//   Sector 0:   header (serial/ID)
//   Sectors 1-2: directory + backup (127 × 0x40-byte CARDDir entries)
//   Sectors 3-4: FAT + backup (u16[SECTOR/2] chain array, big-endian)
//   Sectors 5+:  file data (one sector per allocation unit)
//
//   CARDDir entry (0x40 bytes, all multi-byte fields big-endian):
//     0x00 gameName[4]   0x04 company[2]    0x06 _pad  0x07 bannerFormat
//     0x08 fileName[32]  0x28 time(u32)     0x2C iconAddr(u32)
//     0x30 iconFormat(u16)  0x32 iconSpeed(u16)  0x34 permission
//     0x35 copyTimes     0x36 startBlock(u16)  0x38 length(u16,blocks)
//     0x3A _pad[2]       0x3C commentAddr(u32)
//   Free slot: gameName[0] == 0xFF.
//
//   FAT u16 array (big-endian, sector 3):
//     [0]=0x0000 avail  [1]=checksum  [2]=~checksum
//     [3]=freeBlocks    [4]=lastSlot
//     [k>=5]: next block in chain, 0xFFFF=end, 0x0000=free.
//
// PRIOR ART: runtime/overrides/native_card.cpp — recomp-build variant that
// operates on guest RAM via call_ppc. This file is the native-engine equivalent
// (no guest RAM, no call_ppc, plain host POSIX I/O).

#include <dolphin/card.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// Card geometry constants (matching GC hardware / card.h)
// ---------------------------------------------------------------------------
static constexpr int CSECT      = 8192;   // sector / allocation unit size
static constexpr int CSYSBLKS   = 5;      // header+dir+dirBak+fat+fatBak
static constexpr int CDATABLK   = 5;      // first data block index
static constexpr int CMAXFILES  = 127;    // max directory entries
static constexpr int CDIRENT    = 0x40;   // CARDDir entry size in bytes
static constexpr int CNAMELEN   = 32;     // CARD_FILENAME_MAX

// Default image: 64 total blocks (5 system + 59 user).
static constexpr int  CTOTAL  = 64;
static constexpr long CIMGSZ  = (long)CTOTAL * CSECT;

static constexpr unsigned short FAT_FREE = 0x0000;
static constexpr unsigned short FAT_LAST = 0xFFFF;

// ---------------------------------------------------------------------------
// Per-channel state
// ---------------------------------------------------------------------------
namespace {

struct CardState {
    int   fd         = -1;
    bool  mounted    = false;
    bool  open_failed = false;
    long  image_sz   = 0;
    int   total_blks = 0;
    int   user_blks  = 0;
    unsigned char dir_sec[CSECT] = {};   // sector 1 (big-endian on disk)
    unsigned char fat_sec[CSECT] = {};   // sector 3 (big-endian on disk)
    bool  file_open[CMAXFILES]   = {};
};

static CardState g_cs[2];

// ---- big-endian byte helpers -----------------------------------------------
static unsigned short r16(const unsigned char* p) {
    return (unsigned short)(((unsigned)p[0]<<8)|p[1]); }
static unsigned int r32(const unsigned char* p) {
    return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3]; }
static void w16(unsigned char* p, unsigned short v) { p[0]=v>>8; p[1]=v&0xFF; }
static void w32(unsigned char* p, unsigned int v) {
    p[0]=v>>24; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF; }

// ---- directory entry pointer ------------------------------------------------
static unsigned char* dent(CardState* cs, int n) {
    return cs->dir_sec + (size_t)n * CDIRENT; }

// ---- FAT helpers (big-endian u16 array) ------------------------------------
static unsigned short fat_get(CardState* cs, int blk) {
    return r16(cs->fat_sec + (size_t)blk * 2); }
static void fat_set(CardState* cs, int blk, unsigned short v) {
    w16(cs->fat_sec + (size_t)blk * 2, v); }

static void fat_recompute_checksum(CardState* cs) {
    unsigned short sum = 0;
    for (int i = CDATABLK; i < (int)(CSECT/2); ++i) sum ^= fat_get(cs, i);
    fat_set(cs, 1, sum);
    fat_set(cs, 2, (unsigned short)~sum);
}
static int fat_free_count(CardState* cs) {
    int n = 0;
    for (int i = CDATABLK; i < cs->total_blks; ++i)
        if (fat_get(cs, i) == FAT_FREE) ++n;
    return n;
}
static int fat_alloc(CardState* cs, int nblks) {
    // Collect free blocks; link them into a chain.
    int chain[4096]; int got = 0;
    for (int i = CDATABLK; i < cs->total_blks && got < nblks; ++i)
        if (fat_get(cs, i) == FAT_FREE) chain[got++] = i;
    if (got < nblks) return -1;
    for (int j = 0; j < nblks-1; ++j) fat_set(cs, chain[j], (unsigned short)chain[j+1]);
    fat_set(cs, chain[nblks-1], FAT_LAST);
    return chain[0];
}
// fat_free_chain: would be used by CARDDelete (not in the required 14 symbols).
// Defined here for completeness but not currently called.
[[maybe_unused]] static void fat_free_chain(CardState* cs, int blk) {
    while (blk >= CDATABLK && blk < cs->total_blks) {
        unsigned short nx = fat_get(cs, blk);
        fat_set(cs, blk, FAT_FREE);
        if (nx == FAT_LAST || nx == FAT_FREE) break;
        blk = (int)nx;
    }
}

// ---- sector I/O ------------------------------------------------------------
static bool sec_read(CardState* cs, int blk, void* buf) {
    if (cs->fd < 0 || blk < 0 || blk >= cs->total_blks) return false;
    return pread(cs->fd, buf, CSECT, (off_t)blk*CSECT) == CSECT;
}
static bool sec_write(CardState* cs, int blk, const void* buf) {
    if (cs->fd < 0 || blk < 0 || blk >= cs->total_blks) return false;
    return pwrite(cs->fd, buf, CSECT, (off_t)blk*CSECT) == CSECT;
}
static bool flush_meta(CardState* cs) {
    // Recompute free count.
    int free_cnt = fat_free_count(cs);
    w16(cs->fat_sec + 3*2, (unsigned short)free_cnt);
    bool ok = true;
    ok &= sec_write(cs, 1, cs->dir_sec);
    ok &= sec_write(cs, 2, cs->dir_sec);   // dir backup
    ok &= sec_write(cs, 3, cs->fat_sec);
    ok &= sec_write(cs, 4, cs->fat_sec);   // fat backup
    return ok;
}

// ---- image open / blank-create --------------------------------------------
static bool ensure_open(CardState* cs, int chan) {
    if (cs->fd >= 0)     return true;
    if (cs->open_failed) return false;

    (void)mkdir("scratch", 0755);
    char path[256];
    snprintf(path, sizeof path, "scratch/memcard_chan%d.raw", chan);

    cs->fd = open(path, O_RDWR);
    if (cs->fd < 0) {
        cs->fd = open(path, O_RDWR | O_CREAT, 0644);
        if (cs->fd < 0) {
            fprintf(stderr, "[card] FAILED to create %s: %m\n", path);
            cs->open_failed = true; return false;
        }
        static unsigned char ff[CSECT];
        memset(ff, 0xFF, sizeof ff);
        for (int i = 0; i < CTOTAL; ++i) {
            if (pwrite(cs->fd, ff, CSECT, (off_t)i*CSECT) != CSECT) {
                fprintf(stderr, "[card] blank-write error blk %d\n", i);
                close(cs->fd); cs->fd=-1; cs->open_failed=true; return false;
            }
        }
        fprintf(stderr, "[card] created blank card image %s (%d blocks)\n", path, CTOTAL);
    }
    struct stat st{};
    if (fstat(cs->fd, &st) != 0 || st.st_size < CSECT) {
        close(cs->fd); cs->fd=-1; cs->open_failed=true; return false;
    }
    cs->image_sz   = (long)st.st_size;
    cs->total_blks = (int)(cs->image_sz / CSECT);
    cs->user_blks  = cs->total_blks - CSYSBLKS;
    fprintf(stderr, "[card] opened %s (%d blks, %d user)\n", path, cs->total_blks, cs->user_blks);
    return true;
}

// ---- directory scan -------------------------------------------------------
static int dir_find(CardState* cs, const char* name) {
    for (int i = 0; i < CMAXFILES; ++i) {
        const unsigned char* e = dent(cs, i);
        if (e[0] == 0xFF) continue;
        if (strncmp((const char*)(e + 0x08), name, CNAMELEN) == 0) return i;
    }
    return -1;
}
static int dir_free_slot(CardState* cs) {
    for (int i = 0; i < CMAXFILES; ++i)
        if (dent(cs, i)[0] == 0xFF) return i;
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// extern "C" API — 14 symbols the game links
// ---------------------------------------------------------------------------
extern "C" {

void CARDInit(void) {
    for (int c = 0; c < 2; ++c) {
        CardState* cs = &g_cs[c];
        if (cs->fd >= 0) close(cs->fd);
        *cs = CardState{};
    }
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[chan];
    if (!ensure_open(cs, chan)) return CARD_RESULT_NOCARD;
    if (memSize)    *memSize    = (s32)(cs->image_sz * 8 / (1024*1024));
    if (sectorSize) *sectorSize = CSECT;
    return CARD_RESULT_READY;
}

s32 CARDMount(s32 chan, void* /*workArea*/, CARDCallback /*detachCb*/) {
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[chan];
    if (!ensure_open(cs, chan)) return CARD_RESULT_NOCARD;
    if (cs->mounted) return CARD_RESULT_BUSY;

    if (!sec_read(cs, 1, cs->dir_sec)) return CARD_RESULT_IOERROR;
    if (!sec_read(cs, 3, cs->fat_sec)) return CARD_RESULT_IOERROR;

    cs->mounted = true;
    // A blank (all-0xFF) card has FAT[0] = 0xFFFF.
    // A formatted card always sets FAT[0] = 0x0000 (AVAIL marker).
    // We use FAT[0] NOT FAT[1] (checksum) because the XOR-checksum naturally becomes
    // 0xFFFF when exactly one block is allocated (FAT[5]=0xFFFF, XOR = 0xFFFF), which
    // would falsely trigger a BROKEN report on a valid single-file card.
    if (fat_get(cs, 0) == 0xFFFF) {
        fprintf(stderr, "[card] chan%d: unformatted (blank) — game will format\n", chan);
        return CARD_RESULT_BROKEN;
    }
    fprintf(stderr, "[card] chan%d: mounted (%d blks, %d user, %d free)\n",
            chan, cs->total_blks, cs->user_blks, fat_free_count(cs));
    return CARD_RESULT_READY;
}

s32 CARDUnmount(s32 chan) {
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[chan];
    if (!cs->mounted) return CARD_RESULT_NOCARD;
    cs->mounted = false;
    return CARD_RESULT_READY;
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[chan];
    if (!cs->mounted) return CARD_RESULT_NOCARD;
    int fb = fat_free_count(cs);
    int fs = 0;
    for (int i = 0; i < CMAXFILES; ++i) if (dent(cs, i)[0] == 0xFF) ++fs;
    if (byteNotUsed)  *byteNotUsed  = fb * CSECT;
    if (filesNotUsed) *filesNotUsed = fs;
    return CARD_RESULT_READY;
}

long CARDFormat(long chan) {
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[(int)chan];
    if (!ensure_open(cs, (int)chan)) return CARD_RESULT_NOCARD;

    memset(cs->dir_sec, 0xFF, sizeof cs->dir_sec);   // all slots free
    memset(cs->fat_sec, 0x00, sizeof cs->fat_sec);   // all blocks free

    fat_set(cs, 0, 0x0000);
    fat_set(cs, 3, (unsigned short)(cs->total_blks - CDATABLK));
    fat_set(cs, 4, (unsigned short)CDATABLK);
    fat_recompute_checksum(cs);

    // Minimal header sector.
    unsigned char hdr[CSECT]; memset(hdr, 0xFF, sizeof hdr);
    memcpy(hdr, "SUNBRIGHTCRD", 12);
    sec_write(cs, 0, hdr);

    if (!flush_meta(cs)) return CARD_RESULT_IOERROR;
    cs->mounted = true;
    fprintf(stderr, "[card] chan%ld: formatted (%d user blks)\n", chan, cs->user_blks);
    return CARD_RESULT_READY;
}

long CARDCheck(long chan) {
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[(int)chan];
    if (!cs->mounted) return CARD_RESULT_NOCARD;
    return (fat_get(cs, 0) == 0xFFFF) ? CARD_RESULT_BROKEN : CARD_RESULT_READY;
}

long CARDCreate(long chan, char* fileName, unsigned long size, CARDFileInfo* fileInfo) {
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[(int)chan];
    if (!cs->mounted) return CARD_RESULT_NOCARD;
    if (!fileName || !fileInfo) return CARD_RESULT_FATAL_ERROR;
    if (strlen(fileName) >= (size_t)CNAMELEN) return CARD_RESULT_NAMETOOLONG;
    if (dir_find(cs, fileName) >= 0) return CARD_RESULT_EXIST;

    int slot = dir_free_slot(cs);
    if (slot < 0) return CARD_RESULT_LIMIT;

    int nblks = (int)((size + CSECT - 1) / CSECT);
    if (nblks == 0) nblks = 1;
    int start = fat_alloc(cs, nblks);
    if (start < 0) return CARD_RESULT_INSSPACE;

    unsigned char* e = dent(cs, slot);
    memset(e, 0x00, CDIRENT);
    memset(e + 0x00, 0x20, 6);             // gameName + company = spaces (game fills via SetStatus)
    strncpy((char*)(e + 0x08), fileName, CNAMELEN);
    w32(e + 0x2C, 0xFFFFFFFF);             // iconAddr = unused
    e[0x34] = CARD_ATTR_PUBLIC;            // permission
    w16(e + 0x36, (unsigned short)start);  // startBlock
    w16(e + 0x38, (unsigned short)nblks);  // length (blocks)
    w32(e + 0x3C, 0xFFFFFFFF);             // commentAddr = unused

    // Zero-fill allocated blocks (GC card behavior on create).
    static unsigned char zero[CSECT]; memset(zero, 0, sizeof zero);
    int blk = start;
    while (blk >= CDATABLK && blk < cs->total_blks) {
        sec_write(cs, blk, zero);
        unsigned short nx = fat_get(cs, blk);
        if (nx == FAT_LAST || nx == FAT_FREE) break;
        blk = (int)nx;
    }

    fat_recompute_checksum(cs);
    flush_meta(cs);

    fileInfo->chan   = (s32)chan;
    fileInfo->fileNo = slot;
    fileInfo->offset = 0;
    fileInfo->length = nblks * CSECT;
    fileInfo->iBlock = (u16)start;
    cs->file_open[slot] = true;
    return CARD_RESULT_READY;
}

s32 CARDOpen(s32 chan, char* fileName, CARDFileInfo* fileInfo) {
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[chan];
    if (!cs->mounted) return CARD_RESULT_NOCARD;
    if (!fileName || !fileInfo) return CARD_RESULT_FATAL_ERROR;

    int slot = dir_find(cs, fileName);
    if (slot < 0) return CARD_RESULT_NOFILE;

    const unsigned char* e = dent(cs, slot);
    unsigned short start = (unsigned short)r16(e + 0x36);
    unsigned short nblks = (unsigned short)r16(e + 0x38);

    fileInfo->chan   = chan;
    fileInfo->fileNo = slot;
    fileInfo->offset = 0;
    fileInfo->length = (s32)nblks * CSECT;
    fileInfo->iBlock = start;
    cs->file_open[slot] = true;
    return CARD_RESULT_READY;
}

s32 CARDClose(CARDFileInfo* fileInfo) {
    if (!fileInfo) return CARD_RESULT_FATAL_ERROR;
    if (fileInfo->chan < 0 || fileInfo->chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[fileInfo->chan];
    int slot = fileInfo->fileNo;
    if (slot < 0 || slot >= CMAXFILES) return CARD_RESULT_NOFILE;
    cs->file_open[slot] = false;
    return CARD_RESULT_READY;
}

long CARDRead(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset) {
    if (!fileInfo || !buf || length <= 0) return CARD_RESULT_FATAL_ERROR;
    if (fileInfo->chan < 0 || fileInfo->chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[fileInfo->chan];
    if (!cs->mounted) return CARD_RESULT_NOCARD;
    if (offset < 0 || offset + length > fileInfo->length) return CARD_RESULT_FATAL_ERROR;

    unsigned char* dst = (unsigned char*)buf;
    int remaining = length;
    int blk_off   = offset / CSECT;
    int byte_off  = offset % CSECT;

    int blk = (int)fileInfo->iBlock;
    for (int i = 0; i < blk_off && blk >= CDATABLK; ++i) {
        unsigned short nx = fat_get(cs, blk);
        if (nx == FAT_LAST || nx == FAT_FREE) return CARD_RESULT_IOERROR;
        blk = (int)nx;
    }
    while (remaining > 0) {
        if (blk < CDATABLK || blk >= cs->total_blks) return CARD_RESULT_IOERROR;
        unsigned char sbuf[CSECT];
        if (!sec_read(cs, blk, sbuf)) return CARD_RESULT_IOERROR;
        int take = std::min(remaining, CSECT - byte_off);
        memcpy(dst, sbuf + byte_off, (size_t)take);
        dst += take; remaining -= take; byte_off = 0;
        if (remaining > 0) {
            unsigned short nx = fat_get(cs, blk);
            if (nx == FAT_LAST || nx == FAT_FREE) return CARD_RESULT_IOERROR;
            blk = (int)nx;
        }
    }
    return CARD_RESULT_READY;
}

long CARDWrite(CARDFileInfo* fileInfo, void* buf, long length, long offset) {
    if (!fileInfo || !buf || length <= 0) return CARD_RESULT_FATAL_ERROR;
    if (fileInfo->chan < 0 || fileInfo->chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[fileInfo->chan];
    if (!cs->mounted) return CARD_RESULT_NOCARD;
    if (offset < 0 || offset + (s32)length > fileInfo->length) return CARD_RESULT_FATAL_ERROR;

    const unsigned char* src = (const unsigned char*)buf;
    long remaining = length;
    int blk_off   = (int)offset / CSECT;
    int byte_off  = (int)offset % CSECT;

    int blk = (int)fileInfo->iBlock;
    for (int i = 0; i < blk_off && blk >= CDATABLK; ++i) {
        unsigned short nx = fat_get(cs, blk);
        if (nx == FAT_LAST || nx == FAT_FREE) return CARD_RESULT_IOERROR;
        blk = (int)nx;
    }
    while (remaining > 0) {
        if (blk < CDATABLK || blk >= cs->total_blks) return CARD_RESULT_IOERROR;
        unsigned char sbuf[CSECT];
        if (!sec_read(cs, blk, sbuf)) return CARD_RESULT_IOERROR;
        int put = (int)std::min(remaining, (long)(CSECT - byte_off));
        memcpy(sbuf + byte_off, src, (size_t)put);
        if (!sec_write(cs, blk, sbuf)) return CARD_RESULT_IOERROR;
        src += put; remaining -= put; byte_off = 0;
        if (remaining > 0) {
            unsigned short nx = fat_get(cs, blk);
            if (nx == FAT_LAST || nx == FAT_FREE) return CARD_RESULT_IOERROR;
            blk = (int)nx;
        }
    }
    return CARD_RESULT_READY;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[chan];
    if (!cs->mounted) return CARD_RESULT_NOCARD;
    if (!stat || fileNo < 0 || fileNo >= CMAXFILES) return CARD_RESULT_NOFILE;
    const unsigned char* e = dent(cs, fileNo);
    if (e[0] == 0xFF) return CARD_RESULT_NOFILE;

    memset(stat, 0, sizeof *stat);
    strncpy(stat->fileName, (const char*)(e + 0x08), CNAMELEN);
    unsigned short nblks = (unsigned short)r16(e + 0x38);
    stat->length      = (u32)nblks * CSECT;
    stat->time        = r32(e + 0x28);
    memcpy(stat->gameName, e + 0x00, 4);
    memcpy(stat->company,  e + 0x04, 2);
    stat->bannerFormat = e[0x07];
    stat->iconAddr     = r32(e + 0x2C);
    stat->iconFormat   = (u16)r16(e + 0x30);
    stat->iconSpeed    = (u16)r16(e + 0x32);
    stat->commentAddr  = r32(e + 0x3C);
    return CARD_RESULT_READY;
}

long CARDSetStatus(long chan, long fileNo, CARDStat* stat) {
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    CardState* cs = &g_cs[(int)chan];
    if (!cs->mounted) return CARD_RESULT_NOCARD;
    if (!stat || fileNo < 0 || fileNo >= CMAXFILES) return CARD_RESULT_NOFILE;
    unsigned char* e = dent(cs, (int)fileNo);
    if (e[0] == 0xFF) return CARD_RESULT_NOFILE;
    memcpy(e + 0x00, stat->gameName, 4);
    memcpy(e + 0x04, stat->company,  2);
    e[0x07] = stat->bannerFormat;
    w32(e + 0x28, stat->time);
    w32(e + 0x2C, stat->iconAddr);
    w16(e + 0x30, stat->iconFormat);
    w16(e + 0x32, stat->iconSpeed);
    w32(e + 0x3C, stat->commentAddr);
    flush_meta(cs);
    return CARD_RESULT_READY;
}

} // extern "C"
