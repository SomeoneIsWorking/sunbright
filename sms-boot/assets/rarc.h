// =============================================================================
// Endian-safe Yaz0 (SZS) decompressor + RARC/ARC archive reader.
//
// GameCube on-disk asset formats are BIG-ENDIAN. The decomp parses them by
// casting raw file bytes to C structs (e.g. JKRArchive::SArcHeader in
// decomp/sms/include/JSystem/JKernel/JKRArchive.hpp), which silently
// MISREADS every multi-byte field on a little-endian host. This reader is the
// native, endian-CORRECT replacement: every multi-byte field is read with
// explicit big-endian byte assembly (be16/be32) — never struct overlay.
//
// Portable across x86-64 and arm64 (no reliance on host byte order).
// =============================================================================
#ifndef SMSPORT_ASSETS_RARC_H
#define SMSPORT_ASSETS_RARC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smsport::assets {

// --- Explicit big-endian scalar reads (host-byte-order independent) --------
inline uint16_t be16(const uint8_t* p) {
	return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
inline uint32_t be32(const uint8_t* p) {
	return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
	       | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// --- Yaz0 ('Yaz0' / SZS) decompression -------------------------------------
// Header: magic "Yaz0" (4) + big-endian uncompressed size (4) + 8 reserved.
// Body starts at offset 0x10: groups of 1 flag byte (MSB-first) gating 8 ops:
//   flag bit 1 -> copy one literal byte
//   flag bit 0 -> back-reference: 2 bytes -> dist=((b0&0xF)<<8|b1)+1,
//                 count = (b0>>4 ? (b0>>4)+2 : next_byte+0x12).
// Returns true on success; 'out' is sized to the header's uncompressed length.
bool yaz0_decompress(const uint8_t* src, size_t srclen, std::vector<uint8_t>& out);

// Is the buffer Yaz0-compressed?
inline bool is_yaz0(const uint8_t* src, size_t srclen) {
	return srclen >= 4 && src[0] == 'Y' && src[1] == 'a' && src[2] == 'z'
	       && src[3] == '0';
}

// --- RARC archive parsing ---------------------------------------------------
struct RarcFile {
	std::string name;     // file name (ASCII, from the string table)
	uint32_t    size;     // file data length in bytes
	uint32_t    offset;   // offset of file data within the archive buffer
	const uint8_t* data;  // pointer into the archive buffer (not owned)
};

struct RarcArchive {
	std::vector<RarcFile> files;  // regular files only (directories excluded)
};

// Parse a decompressed RARC archive. 'buf' must remain alive for as long as
// the returned RarcFile::data pointers are used. Returns true on success.
bool rarc_parse(const uint8_t* buf, size_t len, RarcArchive& out);

}  // namespace smsport::assets

#endif  // SMSPORT_ASSETS_RARC_H
