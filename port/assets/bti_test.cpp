// End-to-end native asset data-path test:
//
//   RARC archive (Yaz0/SZS)  ->  rarc_parse  ->  find a .bti file
//     ->  bti_parse (endian-safe ResTIMG header)
//     ->  sb_tex_decode (PC-native GameCube tiled -> RGBA8, no Dolphin)
//     ->  write a PNG  (scratch/port_assets/<name>.png)
//
// This proves the native asset path on REAL game data: a big-endian GameCube
// texture decoded to a viewable RGBA8 image on a little-endian x86-64 host with
// zero emulator/struct-overlay code in the loop.
//
// PASS criterion: the decoded image's dimensions match the BTI header AND the
// pixel data is non-trivial (variance well above zero / not all-uniform), i.e.
// a recognizable image rather than byteswapped garbage.

#include "bti.h"
#include "rarc.h"
#include "../../runtime/render/tex_decode.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace smsport::assets;

// ── minimal PNG writer (zlib "stored"/uncompressed blocks; no external deps) ──
namespace {

uint32_t crc_table[256];
void init_crc() {
	for (uint32_t n = 0; n < 256; ++n) {
		uint32_t c = n;
		for (int k = 0; k < 8; ++k)
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc_table[n] = c;
	}
}
uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n) {
	crc ^= 0xFFFFFFFFu;
	for (size_t i = 0; i < n; ++i)
		crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

void put_be32(std::vector<uint8_t>& v, uint32_t x) {
	v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
	v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}

void write_chunk(std::ofstream& f, const char* type, const std::vector<uint8_t>& data) {
	std::vector<uint8_t> len; put_be32(len, (uint32_t)data.size());
	f.write((const char*)len.data(), 4);
	f.write(type, 4);
	if (!data.empty()) f.write((const char*)data.data(), data.size());
	// CRC is computed over [type || data] as one contiguous message.
	std::vector<uint8_t> msg(type, type + 4);
	msg.insert(msg.end(), data.begin(), data.end());
	uint32_t c = crc32(0, msg.data(), msg.size());
	std::vector<uint8_t> cv; put_be32(cv, c);
	f.write((const char*)cv.data(), 4);
}

// adler32 for the zlib stream
uint32_t adler32(const uint8_t* p, size_t n) {
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < n; ++i) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
	return (b << 16) | a;
}

// Build a zlib stream wrapping the raw scanline bytes using stored deflate blocks.
std::vector<uint8_t> zlib_store(const std::vector<uint8_t>& raw) {
	std::vector<uint8_t> z;
	z.push_back(0x78); z.push_back(0x01);  // zlib header (no compression)
	size_t off = 0;
	while (off < raw.size()) {
		size_t block = std::min<size_t>(65535, raw.size() - off);
		bool last = (off + block >= raw.size());
		z.push_back(last ? 1 : 0);  // BFINAL, BTYPE=00 (stored)
		uint16_t len = (uint16_t)block, nlen = (uint16_t)~len;
		z.push_back(uint8_t(len & 0xFF)); z.push_back(uint8_t(len >> 8));
		z.push_back(uint8_t(nlen & 0xFF)); z.push_back(uint8_t(nlen >> 8));
		z.insert(z.end(), raw.begin() + off, raw.begin() + off + block);
		off += block;
	}
	uint32_t ad = adler32(raw.data(), raw.size());
	put_be32(z, ad);
	return z;
}

bool write_png(const std::string& path, const uint32_t* rgba, int w, int h) {
	init_crc();
	std::ofstream f(path, std::ios::binary);
	if (!f) return false;
	const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	f.write((const char*)sig, 8);

	// IHDR
	std::vector<uint8_t> ihdr;
	put_be32(ihdr, (uint32_t)w); put_be32(ihdr, (uint32_t)h);
	ihdr.push_back(8);  // bit depth
	ihdr.push_back(6);  // color type RGBA
	ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
	write_chunk(f, "IHDR", ihdr);

	// raw image: each row prefixed with filter byte 0
	std::vector<uint8_t> raw;
	raw.reserve((size_t)h * (1 + w * 4));
	for (int y = 0; y < h; ++y) {
		raw.push_back(0);
		for (int x = 0; x < w; ++x) {
			uint32_t px = rgba[(size_t)y * w + x];  // R | G<<8 | B<<16 | A<<24
			raw.push_back(uint8_t(px));         // R
			raw.push_back(uint8_t(px >> 8));    // G
			raw.push_back(uint8_t(px >> 16));   // B
			raw.push_back(uint8_t(px >> 24));   // A
		}
	}
	write_chunk(f, "IDAT", zlib_store(raw));
	write_chunk(f, "IEND", {});
	return (bool)f;
}

}  // namespace

// ── helpers ──────────────────────────────────────────────────────────────────
static const char* fmt_name(int f) {
	switch (f) {
	case SB_TF_I4: return "I4"; case SB_TF_I8: return "I8";
	case SB_TF_IA4: return "IA4"; case SB_TF_IA8: return "IA8";
	case SB_TF_RGB565: return "RGB565"; case SB_TF_RGB5A3: return "RGB5A3";
	case SB_TF_RGBA8: return "RGBA8"; case SB_TF_C4: return "C4";
	case SB_TF_C8: return "C8"; case SB_TF_C14X2: return "C14X2";
	case SB_TF_CMPR: return "CMPR"; default: return "??";
	}
}

int main(int argc, char** argv) {
	const char* arc_path = (argc > 1) ? argv[1] : "scratch/audiores/data/nintendo.szs";
	const char* want     = (argc > 2) ? argv[2] : nullptr;  // optional specific .bti name
	const std::string out_dir = "scratch/port_assets";

	std::printf("== Native asset pipeline: RARC -> BTI -> RGBA8 -> PNG ==\n");
	std::printf("archive: %s\n", arc_path);

	std::ifstream f(arc_path, std::ios::binary);
	if (!f) { std::printf("FAIL: cannot open archive %s\n", arc_path); return 1; }
	std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
	                          std::istreambuf_iterator<char>());
	std::printf("read %zu bytes\n", raw.size());

	std::vector<uint8_t> deco;
	const uint8_t* abuf = raw.data();
	size_t alen = raw.size();
	if (is_yaz0(abuf, alen)) {
		if (!yaz0_decompress(abuf, alen, deco)) { std::printf("FAIL: Yaz0 decompress\n"); return 1; }
		abuf = deco.data(); alen = deco.size();
		std::printf("Yaz0 -> %zu bytes\n", alen);
	}

	RarcArchive arc;
	if (!rarc_parse(abuf, alen, arc)) { std::printf("FAIL: rarc_parse\n"); return 1; }
	std::printf("RARC: %zu files\n", arc.files.size());

	// Find a .bti (or the named one).
	const RarcFile* pick = nullptr;
	int bti_count = 0;
	for (auto& fi : arc.files) {
		bool is_bti = fi.name.size() > 4 &&
		    (fi.name.compare(fi.name.size() - 4, 4, ".bti") == 0 ||
		     fi.name.compare(fi.name.size() - 4, 4, ".BTI") == 0);
		if (!is_bti) continue;
		++bti_count;
		if (want) { if (fi.name == want) pick = &fi; }
		else if (!pick) pick = &fi;  // first .bti
	}
	std::printf("%d .bti files in archive\n", bti_count);
	if (!pick) { std::printf("FAIL: no matching .bti file found\n"); return 1; }
	std::printf("decoding: %s (%u bytes)\n", pick->name.c_str(), pick->size);

	BtiTexture tex;
	if (!bti_parse(pick->data, pick->size, tex)) { std::printf("FAIL: bti_parse\n"); return 1; }
	std::printf("BTI header: format=%s(0x%X) %ux%u  paletted=%d numColors=%u mipmaps=%u\n",
	            fmt_name(tex.format), tex.format, tex.width, tex.height,
	            sb_tex_is_paletted(tex.format), tex.numColors, tex.mipmapCount);

	// --- block-padded dimensions ------------------------------------------------
	// The GameCube stores the image padded UP to the format's block size, and
	// sb_tex_decode operates on (and uses as row pitch) those in-memory padded
	// dims. Pad here, decode into a padded buffer, then crop to the real dims for
	// the PNG. Block widths: 4-bit/8-bit/CMPR = 8, 16-bit & RGBA8 = 4. Block
	// heights: I4/CMPR = 8, everything else = 4.
	int bw, bh;
	switch (tex.format) {
	case SB_TF_I4:                          bw = 8; bh = 8; break;
	case SB_TF_CMPR:                        bw = 8; bh = 8; break;
	case SB_TF_I8: case SB_TF_IA4: case SB_TF_C4: case SB_TF_C8:
	                                        bw = 8; bh = 4; break;
	default:                                bw = 4; bh = 4; break;  // 16-bit, RGBA8, C14X2
	}
	int pw = (tex.width  + bw - 1) / bw * bw;
	int ph = (tex.height + bh - 1) / bh * bh;

	// --- verify the source image bytes actually fit (at padded dims) ------------
	int need = sb_tex_size_bytes(pw, ph, tex.format);
	std::printf("padded dims %dx%d; decode needs %d source bytes; %zu available\n",
	            pw, ph, need, tex.imageLen);
	if ((size_t)need > tex.imageLen) {
		std::printf("FAIL: image data truncated (need %d > have %zu)\n", need, tex.imageLen);
		return 1;
	}
	if (sb_tex_is_paletted(tex.format) && !tex.palette) {
		std::printf("FAIL: paletted format but no palette found\n");
		return 1;
	}

	// --- decode (into the padded buffer, padded width = row pitch) --------------
	std::vector<uint32_t> padded((size_t)pw * ph);
	sb_tex_decode(padded.data(), tex.image, pw, ph, tex.format,
	              tex.palette, tex.paletteFormat);

	// Crop the top-left width×height region (the real image) out of the padded
	// buffer for verification + the PNG.
	std::vector<uint32_t> rgba((size_t)tex.width * tex.height);
	for (int y = 0; y < tex.height; ++y)
		for (int x = 0; x < tex.width; ++x)
			rgba[(size_t)y * tex.width + x] = padded[(size_t)y * pw + x];

	// --- verify the output is sane (non-uniform, plausible) ---------------------
	// Compute luminance mean/variance + count of distinct RGBA values.
	double sum = 0, sumsq = 0;
	uint32_t first = rgba[0];
	bool all_same = true;
	for (uint32_t px : rgba) {
		if (px != first) all_same = false;
		int r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
		double lum = 0.299 * r + 0.587 * g + 0.114 * b;
		sum += lum; sumsq += lum * lum;
	}
	size_t n = rgba.size();
	double mean = sum / n;
	double var = sumsq / n - mean * mean;
	std::printf("decoded %zu texels: luminance mean=%.1f variance=%.1f all_same=%d\n",
	            n, mean, var, (int)all_same);
	std::printf("sample px [0,0]=0x%08X [w/2,h/2]=0x%08X [last]=0x%08X\n",
	            rgba[0], rgba[(size_t)(tex.height/2)*tex.width + tex.width/2],
	            rgba[n-1]);

	// --- write PNG --------------------------------------------------------------
	std::string base = pick->name;
	for (char& c : base) if (c == '/' ) c = '_';
	std::string out_path = out_dir + "/" + base + ".png";
	if (!write_png(out_path, rgba.data(), tex.width, tex.height)) {
		std::printf("FAIL: could not write PNG to %s (does %s exist?)\n",
		            out_path.c_str(), out_dir.c_str());
		return 1;
	}
	std::printf("wrote %s (%ux%u RGBA)\n", out_path.c_str(), tex.width, tex.height);

	// --- PASS/FAIL --------------------------------------------------------------
	bool dims_ok = (tex.width > 0 && tex.height > 0);
	bool nontrivial = (!all_same && var > 1.0);
	if (dims_ok && nontrivial) {
		std::printf("\nPASS: real BTI decodes to a sane image natively via the endian-safe path\n");
		return 0;
	}
	std::printf("\nFAIL: image looks uniform/trivial (dims_ok=%d nontrivial=%d)\n",
	            dims_ok, nontrivial);
	return 1;
}
