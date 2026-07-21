#ifndef SMSPORT_ASSETS_BAS_SWAP_H
#define SMSPORT_ASSETS_BAS_SWAP_H

// .bas (JAudio "binary animated sound") byteswap-on-load.
//
// A .bas file is a flat big-endian table that JAIAnimeSound consumes by DIRECT
// CAST — JAIAnimeSound::initActorAnimSound does `mData = (JAIAnimeSoundData*)data`
// and then reads mCount, per-entry sound ids (u32) and frame times (f32) straight
// out of the mapped file bytes. On our little-endian host every one of those
// fields is byte-reversed, which is how an NPC ended up asking the audio system
// for sound id 0xB89C000 (real MSD_SE_* ids are ~0x28c5), missing the info table,
// and crashing MSoundSE::checkMonoSound on the NULL it got back.
//
// Layout (guest, all big-endian):
//   JAIAnimeSoundData { u16 mCount; u8 pad[6]; JAIAnimeFrameSoundData e[mCount]; }
//   JAIAnimeFrameSoundData (0x20) { u32 id; f32 t0; f32 t1; f32 t2; u32 flags;
//                                   u8 x5[5]; u8 pad[7]; }
// Only the u16 count and the five 32-bit fields per entry need swapping; the
// trailing byte fields are endian-neutral.
//
// Returns a HOST-ENDIAN COPY rather than swapping in place, because
// JKRFileLoader::getGlbResource() hands back a cached buffer that is shared
// between actors and re-fetched on every setAnmSound() — an in-place swap would
// double-swap it back to big-endian on the second call. Copies are cached by
// source pointer, so repeated calls return the same converted buffer (and the
// conversion happens once per file). Mirrors the anm_swap_to_host() precedent
// used for J3D animation blocks.

namespace smsport {
namespace assets {

// `be_data` = pointer to the raw big-endian .bas bytes (may be null -> null).
// Returns a cached host-endian copy, or null if the header is implausible.
const void* bas_to_host(const void* be_data);

} // namespace assets
} // namespace smsport

#endif // SMSPORT_ASSETS_BAS_SWAP_H
