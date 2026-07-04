// Endian-safe Yaz0 + RARC reader. See rarc.h for the rationale.
#include "rarc.h"

namespace smsport::assets {

bool yaz0_decompress(const uint8_t* src, size_t srclen, std::vector<uint8_t>& out) {
	if (!is_yaz0(src, srclen) || srclen < 0x10)
		return false;

	const uint32_t dst_size = be32(src + 4);  // explicit BE uncompressed length
	out.clear();
	out.resize(dst_size);

	size_t  sp = 0x10;   // source read cursor (body starts after the 16B header)
	size_t  dp = 0;      // destination write cursor
	uint8_t flags = 0;   // current group's flag byte
	int     bits_left = 0;

	while (dp < dst_size) {
		if (bits_left == 0) {
			if (sp >= srclen)
				return false;
			flags = src[sp++];
			bits_left = 8;
		}

		if (flags & 0x80) {
			// Literal byte.
			if (sp >= srclen)
				return false;
			out[dp++] = src[sp++];
		} else {
			// Back-reference: two bytes -> distance, plus optional count byte.
			if (sp + 1 >= srclen)
				return false;
			const uint8_t b0 = src[sp];
			const uint8_t b1 = src[sp + 1];
			sp += 2;
			const uint32_t dist  = ((static_cast<uint32_t>(b0 & 0x0F) << 8) | b1) + 1;
			uint32_t       count = static_cast<uint32_t>(b0 >> 4);
			if (count == 0) {
				if (sp >= srclen)
					return false;
				count = static_cast<uint32_t>(src[sp++]) + 0x12;
			} else {
				count += 2;
			}
			if (dist > dp)  // would read before the start of output
				return false;
			// Overlapping copy: must be byte-by-byte (run-length semantics).
			for (uint32_t i = 0; i < count && dp < dst_size; ++i) {
				out[dp] = out[dp - dist];
				++dp;
			}
		}

		flags <<= 1;
		--bits_left;
	}
	return true;
}

bool rarc_parse(const uint8_t* buf, size_t len, RarcArchive& out) {
	out.files.clear();
	if (len < 0x40)
		return false;
	if (!(buf[0] == 'R' && buf[1] == 'A' && buf[2] == 'R' && buf[3] == 'C'))
		return false;

	// --- RARC header (all fields big-endian) -------------------------------
	//   +0x00 'RARC'
	//   +0x04 file length
	//   +0x08 header length (0x20)
	//   +0x0C file-data offset (relative to end of 0x20 header)
	// Then the SArcDataInfo block (the "info block", at +0x20):
	//   +0x20 num_nodes (directories)
	//   +0x24 node table offset      (relative to +0x20)
	//   +0x28 num_file_entries
	//   +0x2C file-entry table offset (relative to +0x20)
	//   +0x30 string table length
	//   +0x34 string table offset    (relative to +0x20)
	const uint32_t data_off  = be32(buf + 0x0C) + 0x20;
	const uint32_t num_nodes = be32(buf + 0x20);
	const uint32_t node_off  = be32(buf + 0x24) + 0x20;
	const uint32_t ent_off   = be32(buf + 0x2C) + 0x20;
	const uint32_t str_off   = be32(buf + 0x34) + 0x20;

	if (node_off > len || ent_off > len || str_off > len)
		return false;

	// Walk each directory node; each owns a contiguous run of file entries.
	for (uint32_t ni = 0; ni < num_nodes; ++ni) {
		const size_t n = static_cast<size_t>(node_off) + ni * 0x10;
		if (n + 0x10 > len)
			return false;
		// Node layout: +0x00 id(4) +0x04 nameOff(4) +0x08 nameHash(2)
		//              +0x0A file count(2) +0x0C first file index(4)
		const uint16_t count = be16(buf + n + 0x0A);
		const uint32_t first = be32(buf + n + 0x0C);

		for (uint32_t fi = 0; fi < count; ++fi) {
			const size_t e = static_cast<size_t>(ent_off) + (first + fi) * 0x14;
			if (e + 0x14 > len)
				return false;
			// File-entry layout (SDIFileEntry on disk, 0x14 bytes):
			//   +0x00 fileID(2) +0x02 nameHash(2)
			//   +0x04 flags(1 hi byte) | nameOffset(3 lo bytes)
			//   +0x08 data offset(4)   +0x0C data size(4)
			const uint16_t fid       = be16(buf + e + 0x00);
			const uint32_t flag_name = be32(buf + e + 0x04);
			const uint8_t  flags     = static_cast<uint8_t>(flag_name >> 24);
			const uint32_t name_o    = flag_name & 0x00FFFFFF;
			const uint32_t doff      = be32(buf + e + 0x08);
			const uint32_t dsize     = be32(buf + e + 0x0C);

			// Directory entries have fileID 0xFFFF and the directory flag (0x02).
			if (fid == 0xFFFF || (flags & 0x02))
				continue;

			// Read the NUL-terminated name from the string table.
			const size_t name_start = static_cast<size_t>(str_off) + name_o;
			if (name_start >= len)
				return false;
			size_t z = name_start;
			while (z < len && buf[z] != '\0')
				++z;
			std::string name(reinterpret_cast<const char*>(buf + name_start),
			                 z - name_start);

			const size_t data_start = static_cast<size_t>(data_off) + doff;
			if (data_start + dsize > len)
				return false;

			RarcFile f;
			f.name   = std::move(name);
			f.size   = dsize;
			f.offset = data_off + doff;
			f.data   = buf + data_start;
			out.files.push_back(std::move(f));
		}
	}
	return true;
}

}  // namespace smsport::assets
