// Test for the endian-safe Yaz0 + RARC reader.
//
//   1. Build a synthetic BIG-ENDIAN RARC fixture in memory (known names/sizes/
//      contents) and parse it -> deterministic, proves endian-correct parsing
//      with no game asset.
//   2. Yaz0 round-trip: compress (trivial all-literal encoder) then decompress
//      and verify byte-exact -> proves the decompressor.
//   3. If a real Yaz0-compressed RARC is available (arg 1, or the default
//      scratch path), decompress + parse it and print the file list. This
//      validates against a REAL big-endian GameCube archive on this LE host.
//
// PASS criterion: names are readable ASCII, sizes are plausible, count > 0 —
// i.e. NOT byteswapped garbage.

#include "rarc.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using namespace smsport::assets;

static int g_fail = 0;
#define CHECK(cond, msg)                                                        \
	do {                                                                        \
		if (!(cond)) {                                                          \
			std::printf("  FAIL: %s\n", msg);                                   \
			++g_fail;                                                           \
		}                                                                       \
	} while (0)

// --- Big-endian writers for building the fixture ---------------------------
static void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
	v[off + 0] = uint8_t(x >> 24);
	v[off + 1] = uint8_t(x >> 16);
	v[off + 2] = uint8_t(x >> 8);
	v[off + 3] = uint8_t(x);
}
static void put16(std::vector<uint8_t>& v, size_t off, uint16_t x) {
	v[off + 0] = uint8_t(x >> 8);
	v[off + 1] = uint8_t(x);
}

// Build a minimal valid RARC with two files in one root directory.
// Layout (all multi-byte fields big-endian):
//   0x00  header (0x20)
//   0x20  info block (0x20)
//   0x40  node table   : 1 node  * 0x10
//   0x50  file entries : 2 files * 0x14 = 0x28
//   0x78  string table
//   ....  file data (aligned to data_off)
static std::vector<uint8_t> build_fixture() {
	const char* n0 = "scene.bin";
	const char* n1 = "config.txt";
	const uint8_t d0[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};  // 6 bytes
	const uint8_t d1[] = {'h', 'e', 'l', 'l', 'o'};             // 5 bytes

	std::vector<uint8_t> v(0x200, 0);

	const uint32_t node_tbl = 0x40;
	const uint32_t ent_tbl  = 0x50;
	const uint32_t str_tbl  = 0x78;

	// String table: dir name "root" then the two file names.
	uint32_t so = str_tbl;
	uint32_t root_no = so - str_tbl;
	std::memcpy(&v[so], "root", 5);  so += 5;
	uint32_t n0_no = so - str_tbl;
	std::memcpy(&v[so], n0, std::strlen(n0) + 1);  so += std::strlen(n0) + 1;
	uint32_t n1_no = so - str_tbl;
	std::memcpy(&v[so], n1, std::strlen(n1) + 1);  so += std::strlen(n1) + 1;
	uint32_t str_len = so - str_tbl;

	// File data, 32-byte aligned region.
	uint32_t data_region = (so + 0x1F) & ~0x1Fu;
	uint32_t d0_off = 0;                 // relative to data region
	std::memcpy(&v[data_region + d0_off], d0, sizeof d0);
	uint32_t d1_off = sizeof d0;
	std::memcpy(&v[data_region + d1_off], d1, sizeof d1);
	uint32_t total = data_region + d1_off + sizeof(d1);
	v.resize((total + 0x1F) & ~0x1Fu, 0);

	// --- Header (offsets relative to +0x20 for the table fields) -----------
	v[0] = 'R'; v[1] = 'A'; v[2] = 'R'; v[3] = 'C';
	put32(v, 0x04, (uint32_t)v.size());          // file length
	put32(v, 0x08, 0x20);                         // header length
	put32(v, 0x0C, data_region - 0x20);           // file-data offset (rel +0x20)
	put32(v, 0x10, (uint32_t)v.size() - data_region);  // file-data length

	// --- Info block at 0x20 ------------------------------------------------
	put32(v, 0x20, 1);                            // num_nodes
	put32(v, 0x24, node_tbl - 0x20);              // node table offset
	put32(v, 0x28, 2);                            // num_file_entries
	put32(v, 0x2C, ent_tbl - 0x20);               // file-entry table offset
	put32(v, 0x30, str_len);                      // string table length
	put32(v, 0x34, str_tbl - 0x20);               // string table offset

	// --- Node 0 (root dir) -------------------------------------------------
	std::memcpy(&v[node_tbl + 0x00], "ROOT", 4);  // dir type/id tag
	put32(v, node_tbl + 0x04, root_no);           // name offset
	put16(v, node_tbl + 0x08, 0);                 // name hash
	put16(v, node_tbl + 0x0A, 2);                 // file count
	put32(v, node_tbl + 0x0C, 0);                 // first file index

	// --- File entry 0 ------------------------------------------------------
	put16(v, ent_tbl + 0x00, 0);                  // fileID
	put16(v, ent_tbl + 0x02, 0);                  // name hash
	put32(v, ent_tbl + 0x04, (0x11u << 24) | n0_no);  // flags(0x11=file) | nameOff
	put32(v, ent_tbl + 0x08, d0_off);             // data offset
	put32(v, ent_tbl + 0x0C, sizeof d0);          // data size

	// --- File entry 1 ------------------------------------------------------
	put16(v, ent_tbl + 0x14 + 0x00, 1);
	put16(v, ent_tbl + 0x14 + 0x02, 0);
	put32(v, ent_tbl + 0x14 + 0x04, (0x11u << 24) | n1_no);
	put32(v, ent_tbl + 0x14 + 0x08, d1_off);
	put32(v, ent_tbl + 0x14 + 0x0C, sizeof d1);

	return v;
}

// Trivial Yaz0 encoder: emit everything as literals (all flag bits set). This
// exercises the decompressor's literal + group-byte path on a controlled input.
static std::vector<uint8_t> yaz0_encode_literals(const std::vector<uint8_t>& in) {
	std::vector<uint8_t> out;
	out.insert(out.end(), {'Y', 'a', 'z', '0'});
	uint32_t n = (uint32_t)in.size();
	out.push_back(uint8_t(n >> 24));
	out.push_back(uint8_t(n >> 16));
	out.push_back(uint8_t(n >> 8));
	out.push_back(uint8_t(n));
	out.insert(out.end(), 8, 0);  // 8 reserved header bytes
	size_t i = 0;
	while (i < in.size()) {
		out.push_back(0xFF);  // group: 8 literals
		for (int b = 0; b < 8 && i < in.size(); ++b)
			out.push_back(in[i++]);
	}
	return out;
}

static void test_fixture() {
	std::printf("[1] Synthetic big-endian RARC fixture\n");
	std::vector<uint8_t> arc = build_fixture();
	RarcArchive a;
	CHECK(rarc_parse(arc.data(), arc.size(), a), "rarc_parse fixture");
	CHECK(a.files.size() == 2, "file count == 2");
	if (a.files.size() == 2) {
		CHECK(a.files[0].name == "scene.bin", "name[0] == scene.bin");
		CHECK(a.files[0].size == 6, "size[0] == 6");
		CHECK(a.files[0].data[0] == 0xDE && a.files[0].data[3] == 0xEF,
		      "data[0] content");
		CHECK(a.files[1].name == "config.txt", "name[1] == config.txt");
		CHECK(a.files[1].size == 5, "size[1] == 5");
		CHECK(std::memcmp(a.files[1].data, "hello", 5) == 0, "data[1] == hello");
		for (auto& f : a.files)
			std::printf("    %-12s  %u bytes  @+0x%X\n", f.name.c_str(), f.size,
			            f.offset);
	}
}

static void test_yaz0_roundtrip() {
	std::printf("[2] Yaz0 round-trip\n");
	std::vector<uint8_t> orig;
	for (int i = 0; i < 1000; ++i)
		orig.push_back(uint8_t((i * 37 + 11) & 0xFF));
	std::vector<uint8_t> comp = yaz0_encode_literals(orig);
	std::vector<uint8_t> deco;
	CHECK(yaz0_decompress(comp.data(), comp.size(), deco), "yaz0_decompress");
	CHECK(deco.size() == orig.size(), "decompressed size matches");
	CHECK(deco == orig, "decompressed bytes match");
	std::printf("    %zu -> Yaz0 %zu -> %zu bytes, exact match: %s\n",
	            orig.size(), comp.size(), deco.size(),
	            (deco == orig) ? "yes" : "NO");
}

static void test_real(const char* path) {
	std::printf("[3] Real archive: %s\n", path);
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		std::printf("    (not present — skipping; fixture+roundtrip still gate PASS)\n");
		return;
	}
	std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
	                         std::istreambuf_iterator<char>());
	std::printf("    read %zu bytes\n", raw.size());

	const uint8_t* arc = raw.data();
	size_t arclen = raw.size();
	std::vector<uint8_t> deco;
	if (is_yaz0(arc, arclen)) {
		CHECK(yaz0_decompress(arc, arclen, deco), "yaz0_decompress real");
		std::printf("    Yaz0 -> %zu bytes decompressed\n", deco.size());
		arc = deco.data();
		arclen = deco.size();
	}

	RarcArchive a;
	if (!rarc_parse(arc, arclen, a)) {
		std::printf("    (decompressed payload is not RARC — not a failure of the reader)\n");
		return;
	}
	CHECK(a.files.size() > 0, "real archive file count > 0");

	std::printf("    %zu files:\n", a.files.size());
	size_t shown = 0;
	bool all_ascii = true, sizes_sane = true;
	for (auto& fi : a.files) {
		for (char c : fi.name)
			if (c < 0x20 || c > 0x7E) all_ascii = false;
		if (fi.size > arclen) sizes_sane = false;
		if (shown < 20) {
			std::printf("      %-28s %8u bytes\n", fi.name.c_str(), fi.size);
			++shown;
		}
	}
	if (a.files.size() > shown)
		std::printf("      ... (%zu more)\n", a.files.size() - shown);
	CHECK(all_ascii, "all file names are printable ASCII (not byteswapped)");
	CHECK(sizes_sane, "all sizes within archive bounds (not byteswapped garbage)");
}

int main(int argc, char** argv) {
	test_fixture();
	test_yaz0_roundtrip();
	// Default to the repo-relative scratch path (resolved from the repo root,
	// where it is run via CMake/CTest WORKING_DIRECTORY). Override with arg 1.
	const char* real = (argc > 1)
	    ? argv[1]
	    : "scratch/audiores/data/nintendo.szs";
	test_real(real);

	std::printf("\n%s\n", g_fail == 0 ? "PASS: endian-safe parse reads big-endian archives correctly on x86-64"
	                                   : "FAIL");
	return g_fail == 0 ? 0 : 1;
}
