// tex_decode_selftest — validates the native GC texture decoder (tex_decode.cpp)
// byte-for-byte against Dolphin's decoder (the oracle for the math). This is the
// ONLY file that pairs our clean-room decoder with Dolphin; tex_decode.{h,cpp}
// stay Dolphin-free. Driven on demand via the /tex probe endpoint.
//
// For each (format × size × tlut-format), it fills a deterministic pseudo-random
// source (+ TLUT for paletted formats), decodes with BOTH decoders into RGBA8,
// and compares every texel. Zero mismatched texels across all cases = parity.

#include "tex_decode.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef HAVE_DOLPHIN_MEMMAP
#include "VideoCommon/TextureDecoder.h"

namespace {

// Deterministic RNG so runs are reproducible.
struct Rng { uint32_t s; uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; } };

struct Case { int fmt; const char* name; bool paletted; };
const Case kCases[] = {
    {SB_TF_I4, "I4", false},   {SB_TF_I8, "I8", false},   {SB_TF_IA4, "IA4", false},
    {SB_TF_IA8, "IA8", false}, {SB_TF_RGB565, "RGB565", false}, {SB_TF_RGB5A3, "RGB5A3", false},
    {SB_TF_RGBA8, "RGBA8", false},
    {SB_TF_C4, "C4", true},    {SB_TF_C8, "C8", true},    {SB_TF_C14X2, "C14X2", true},
    {SB_TF_CMPR, "CMPR", false},
};
// Block-padded dimensions (multiples of 8 in both axes so every format's tiling
// is exact — the real in-memory texture dimensions are always block-padded).
const int kSizes[][2] = {{8, 8}, {16, 16}, {32, 8}, {8, 32}, {16, 24}, {64, 64}, {40, 16}};
const int kTluts[] = {SB_TL_IA8, SB_TL_RGB565, SB_TL_RGB5A3};

}  // namespace

// Returns number of FAILING cases (0 = full parity). Writes a human report.
int sb_tex_selftest(char* out, int cap) {
    int pos = 0, fails = 0, cases = 0;
    auto app = [&](const char* fmt, ...) {
        if (pos >= cap) return;
        va_list ap; va_start(ap, fmt);
        pos += vsnprintf(out + pos, cap - pos, fmt, ap);
        va_end(ap);
    };

    Rng rng{0x12345678u};
    std::vector<uint8_t> tlut(16384 * 2);
    for (auto& b : tlut) b = (uint8_t)rng.next();

    std::vector<uint8_t> src;
    std::vector<uint32_t> mine, dol;

    for (const auto& c : kCases) {
        const int ntlut = c.paletted ? 3 : 1;
        for (const auto& sz : kSizes) {
            for (int ti = 0; ti < ntlut; ti++) {
                const int w = sz[0], h = sz[1];
                const int srcbytes = sb_tex_size_bytes(w, h, c.fmt);
                src.resize(srcbytes);
                for (int i = 0; i < srcbytes; i++) src[i] = (uint8_t)rng.next();
                mine.assign((size_t)w * h, 0);
                dol.assign((size_t)w * h, 0);

                const int tl = kTluts[ti];
                sb_tex_decode(mine.data(), src.data(), w, h, c.fmt,
                              c.paletted ? tlut.data() : nullptr, tl);
                TexDecoder_Decode(reinterpret_cast<uint8_t*>(dol.data()), src.data(), w, h,
                                  static_cast<TextureFormat>(c.fmt),
                                  c.paletted ? tlut.data() : nullptr,
                                  static_cast<TLUTFormat>(tl));

                cases++;
                int bad = 0, firstbad = -1;
                for (int i = 0; i < w * h; i++)
                    if (mine[i] != dol[i]) { if (firstbad < 0) firstbad = i; bad++; }
                if (bad) {
                    fails++;
                    app("FAIL %-6s %2dx%-2d tlut=%d : %d/%d texels differ (first @%d: mine=%08x dol=%08x)\n",
                        c.name, w, h, c.paletted ? tl : -1, bad, w * h, firstbad,
                        mine[firstbad], dol[firstbad]);
                }
            }
        }
    }
    app("tex_selftest: %d cases, %d failing -> %s\n", cases, fails,
        fails == 0 ? "PARITY-OK" : "MISMATCH");
    return fails;
}

#else
int sb_tex_selftest(char* out, int cap) {
    snprintf(out, cap, "tex_selftest: Dolphin oracle unavailable (no HAVE_DOLPHIN_MEMMAP)\n");
    return -1;
}
#endif
