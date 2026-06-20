// dvd_disc.h — disc backend installation for the DVD seam (native/platform/dvd_impl.cpp).
//
// The DVD seam reads raw disc bytes through a pluggable DiscReadFn so it stays
// GameCube-free: the platform layer installs a real backend (a plain GCM/ISO reader,
// or later an RVZ reader) and the unit tests install an in-memory synthetic disc.
// The GC FST (file-system table) is either loaded from the disc boot header by
// sb_dvd_init_from_disc(), or installed directly by sb_dvd_set_fst() (tests / a
// pre-extracted FST).
#pragma once
#include <dolphin/types.h>

extern "C" {

// Read `length` bytes at absolute disc byte `offset` into `dst`. Return true on
// success. `user` is the opaque context passed to sb_dvd_set_disc_source.
typedef bool (*DiscReadFn)(void* dst, u32 length, u32 offset, void* user);

// Install the raw-disc backend. Must be set before DVDInit/reads do anything useful.
void sb_dvd_set_disc_source(DiscReadFn fn, void* user);

// Install the FST directly (caller-owned bytes copied in). GC FST layout: 12-byte
// entries (big-endian) followed by the name string table. Used by tests and by a
// caller that already has the FST in hand.
void sb_dvd_set_fst(const void* fstData, u32 fstSize);

// Read the disc boot header (offset 0x420), pull FSTOffset(0x424)/FSTSize(0x428),
// load the FST via the installed disc source. Returns true on success. Called by
// DVDInit when a disc source is present.
bool sb_dvd_init_from_disc(void);

// Open a PLAIN GameCube disc image (.gcm/.iso, uncompressed) as the disc backend via
// host fopen — pure PC, no Dolphin. Installs the disc source + loads the FST. Returns
// false if the file can't be opened. (RVZ/compressed images are NOT handled here — a
// separate decompressing reader plugs into the same DiscReadFn when needed.)
bool sb_platform_open_gcm(const char* path);

} // extern "C"
