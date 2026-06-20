// dvd_impl.cpp — native PC implementation of the GameCube DVD filesystem seam (E3).
//
// Implements the 13 unresolved DVD* symbols the game links. The GC disc is a flat
// byte image with a boot header (offset 0x420) pointing at the FST (file-system
// table): 12-byte big-endian entries + a name string table. The seam parses the FST
// and serves file lookup / open / read against a pluggable disc backend (dvd_disc.h)
// so it stays GameCube-free — a plain GCM/ISO reader or the unit-test synthetic disc
// plugs in identically.
//
// ASYNC MODEL: like native_card.cpp, async reads complete SYNCHRONOUSLY before the
// call returns, then invoke the callback — there is no real DVD hardware / interrupt
// to wait on, so the SDK's __DVDSync-style waits never block. Command/drive status
// always reports "done/ready".
//
// FST format (matches tools/jingle/jingle_extract.cpp, all big-endian):
//   entry[0]      = root dir; its length field (bytes 8..11) = total entry count.
//   file entry:   byte0=0; bytes1..3=nameOffset; bytes4..7=disc byte offset;
//                 bytes8..11=length.
//   dir entry:    byte0=1; bytes1..3=nameOffset; bytes4..7=parent index;
//                 bytes8..11=next index (one past the dir's last descendant).
//   string table follows entryCount*12 bytes; names are NUL-terminated.

#include <dolphin/dvd.h>
#include "dvd_disc.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace {

DiscReadFn g_disc = nullptr;
void* g_disc_user = nullptr;

std::vector<uint8_t> g_fst;   // owned FST bytes
u32 g_numEntries = 0;
const char* g_strTab = nullptr;
s32 g_currentDir = 0;         // entrynum of cwd (root)

u32 be32(const uint8_t* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

const uint8_t* entry(u32 i) { return g_fst.data() + (size_t)i * 12; }
bool   ent_is_dir(u32 i)    { return entry(i)[0] == 1; }
u32    ent_name_off(u32 i)  { return be32(entry(i)) & 0x00FFFFFF; }
u32    ent_file_off(u32 i)  { return be32(entry(i) + 4); }
u32    ent_file_len(u32 i)  { return be32(entry(i) + 8); }
u32    ent_dir_next(u32 i)  { return be32(entry(i) + 8); }
const char* ent_name(u32 i) { return g_strTab + ent_name_off(i); }

// Find a direct child named [name, name+len) within dir entry `dir` (case-
// insensitive, matching the SDK). Returns entrynum or -1. Skips nested subtrees so
// only DIRECT children match.
s32 find_child(s32 dir, const char* name, size_t len) {
    u32 end = ent_is_dir((u32)dir) ? ent_dir_next((u32)dir) : 0;
    u32 i = (u32)dir + 1;
    while (i < end) {
        const char* cn = ent_name(i);
        size_t cl = std::strlen(cn);
        if (cl == len) {
            bool eq = true;
            for (size_t k = 0; k < len; ++k) {
                char a = name[k], b = cn[k];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { eq = false; break; }
            }
            if (eq) return (s32)i;
        }
        // advance: skip a subdirectory's whole subtree
        i = ent_is_dir(i) ? ent_dir_next(i) : i + 1;
    }
    return -1;
}

} // namespace

extern "C" {

// ---- disc backend installation (dvd_disc.h) -------------------------------
void sb_dvd_set_disc_source(DiscReadFn fn, void* user) {
    g_disc = fn; g_disc_user = user;
}

void sb_dvd_set_fst(const void* fstData, u32 fstSize) {
    g_fst.assign((const uint8_t*)fstData, (const uint8_t*)fstData + fstSize);
    g_numEntries = fstSize >= 12 ? be32(g_fst.data() + 8) : 0;
    g_strTab = (const char*)(g_fst.data() + (size_t)g_numEntries * 12);
    g_currentDir = 0;
}

bool sb_dvd_init_from_disc(void) {
    if (!g_disc) return false;
    uint8_t boot[0x440];
    if (!g_disc(boot, sizeof(boot), 0, g_disc_user)) return false;
    u32 fstOff = be32(boot + 0x424);
    u32 fstSize = be32(boot + 0x428);
    if (fstSize == 0 || fstSize > (64u << 20)) return false;
    std::vector<uint8_t> buf(fstSize);
    if (!g_disc(buf.data(), fstSize, fstOff, g_disc_user)) return false;
    sb_dvd_set_fst(buf.data(), fstSize);
    return true;
}

// ---- init / status --------------------------------------------------------
void DVDInit(void) {
    if (g_disc && g_fst.empty()) sb_dvd_init_from_disc();
}
BOOL DVDCheckDisk(void) { return TRUE; }
long DVDGetDriveStatus(void) { return 0; }                       // DVD_STATE_END (ready)
s32  DVDGetCommandBlockStatus(struct DVDCommandBlock*) { return 0; }  // done

// ---- path -> entrynum -----------------------------------------------------
s32 DVDConvertPathToEntrynum(char* pathPtr) {
    if (g_fst.empty() || !pathPtr) return -1;
    const char* p = pathPtr;
    s32 cur = (*p == '/') ? 0 : g_currentDir;
    if (*p == '/') ++p;
    while (*p) {
        // extract token up to '/' or end
        const char* start = p;
        while (*p && *p != '/') ++p;
        size_t len = (size_t)(p - start);
        if (*p == '/') ++p;
        if (len == 0) continue;
        if (len == 1 && start[0] == '.') continue;          // "."
        if (len == 2 && start[0] == '.' && start[1] == '.') { // ".."
            if (cur != 0) cur = (s32)ent_file_off((u32)cur); // dir parent @ bytes4..7
            continue;
        }
        if (!ent_is_dir((u32)cur)) return -1;
        s32 child = find_child(cur, start, len);
        if (child < 0) return -1;
        cur = child;
    }
    return cur;
}

// ---- open / close ---------------------------------------------------------
BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo) {
    if (entrynum < 0 || (u32)entrynum >= g_numEntries) return FALSE;
    if (ent_is_dir((u32)entrynum)) return FALSE;
    std::memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->startAddr = ent_file_off((u32)entrynum);
    fileInfo->length = ent_file_len((u32)entrynum);
    fileInfo->cb.command = -1;  // idle
    fileInfo->cb.state = 0;     // DVD_STATE_END
    return TRUE;
}

BOOL DVDOpen(char* fileName, DVDFileInfo* fileInfo) {
    s32 e = DVDConvertPathToEntrynum(fileName);
    if (e < 0 || ent_is_dir((u32)e)) return FALSE;
    return DVDFastOpen(e, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
    if (fileInfo) { fileInfo->startAddr = 0; fileInfo->length = 0; }
    return TRUE;
}

// ---- read -----------------------------------------------------------------
// Synchronous read of [offset, offset+length) within the file into addr.
// Returns bytes read, or DVD_RESULT_FATAL_ERROR (-1).
long DVDReadPrio(DVDFileInfo* fileInfo, void* addr, long length, long offset, long /*prio*/) {
    if (!fileInfo || !g_disc) return DVD_RESULT_FATAL_ERROR;
    if (offset < 0 || (u32)offset > fileInfo->length) return DVD_RESULT_FATAL_ERROR;
    long avail = (long)fileInfo->length - offset;
    if (length > avail) length = avail;
    if (length <= 0) return 0;
    if (!g_disc(addr, (u32)length, fileInfo->startAddr + (u32)offset, g_disc_user))
        return DVD_RESULT_FATAL_ERROR;
    fileInfo->cb.transferredSize = (u32)length;
    return length;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                      DVDCallback callback, s32 prio) {
    long r = DVDReadPrio(fileInfo, addr, length, offset, prio);
    fileInfo->cb.state = 0;  // done
    if (callback) callback((s32)r, fileInfo);  // synchronous completion
    return r >= 0 ? TRUE : FALSE;
}

// Streaming prepare (THP/DTK ADPCM). No streaming hardware natively; report ready.
BOOL DVDPrepareStreamAsync(DVDFileInfo* fileInfo, u32 /*length*/, u32 /*offset*/,
                           DVDCallback callback) {
    if (callback) callback(0, fileInfo);
    return TRUE;
}

// ---- directories ----------------------------------------------------------
BOOL DVDChangeDir(char* dirName) {
    s32 e = DVDConvertPathToEntrynum(dirName);
    if (e < 0 || !ent_is_dir((u32)e)) return FALSE;
    g_currentDir = e;
    return TRUE;
}

int DVDOpenDir(char* dirName, DVDDir* dir) {
    s32 e = DVDConvertPathToEntrynum(dirName);
    if (e < 0 || !ent_is_dir((u32)e)) return FALSE;
    dir->entryNum = (u32)e;
    dir->location = (u32)e + 1;                 // first child
    dir->next = ent_dir_next((u32)e);           // one past last descendant
    return TRUE;
}

int DVDReadDir(DVDDir* dir, DVDDirEntry* dirent) {
    if (dir->location >= dir->next) return FALSE;
    u32 i = dir->location;
    dirent->entryNum = i;
    dirent->isDir = ent_is_dir(i) ? TRUE : FALSE;
    dirent->name = (char*)ent_name(i);
    dir->location = ent_is_dir(i) ? ent_dir_next(i) : i + 1;  // skip subtrees
    return TRUE;
}

int DVDCloseDir(DVDDir* dir) { (void)dir; return TRUE; }

} // extern "C"
