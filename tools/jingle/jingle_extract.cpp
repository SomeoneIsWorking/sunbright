// Standalone PC-native proof tool: read SMS audio data straight from the ROM, no emulation.
// Step 1 (this tool): open the RVZ via DiscIO, parse the GC FST, list / extract files
// (AudioRes init data + wave archives) to scratch/audiores/.
//
// Usage: sunbright-jingle <rom.rvz> [--list] [--extract <prefix> <outdir>]
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "DiscIO/Blob.h"
#include "DiscIO/Volume.h"

using u8 = uint8_t;
using u32 = uint32_t;

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

struct Entry {
    bool is_dir;
    std::string path;
    u32 offset, size;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom> [--list] [--extract <prefix> <outdir>]\n", argv[0]);
        return 1;
    }
    auto vol = DiscIO::CreateVolume(argv[1]);
    if (!vol) { fprintf(stderr, "cannot open volume: %s\n", argv[1]); return 1; }
    const DiscIO::Partition part = DiscIO::PARTITION_NONE;

    u8 boot[0x440];
    if (!vol->Read(0, sizeof(boot), boot, part)) { fprintf(stderr, "boot read failed\n"); return 1; }
    const u32 fst_off = be32(boot + 0x424);
    const u32 fst_sz  = be32(boot + 0x428);
    std::vector<u8> fst(fst_sz);
    if (!vol->Read(fst_off, fst_sz, fst.data(), part)) { fprintf(stderr, "fst read failed\n"); return 1; }

    const u32 n = be32(fst.data() + 8);
    const u8* str_table = fst.data() + n * 12;
    std::vector<Entry> entries(n);
    std::vector<std::string> path_stack = {""};
    std::vector<u32> dir_end = {n};
    for (u32 i = 1; i < n; i++) {
        while (path_stack.size() > 1 && i >= dir_end.back()) { path_stack.pop_back(); dir_end.pop_back(); }
        const u8* e = fst.data() + i * 12;
        Entry& en = entries[i];
        en.is_dir = e[0];
        const u32 name_off = ((u32)e[1] << 16) | ((u32)e[2] << 8) | e[3];
        en.path = path_stack.back() + "/" + (const char*)(str_table + name_off);
        en.offset = be32(e + 4);
        en.size = be32(e + 8);
        if (en.is_dir) { path_stack.push_back(en.path); dir_end.push_back(en.size); }
    }

    const bool list = argc >= 3 && !strcmp(argv[2], "--list");
    const char* prefix = nullptr; const char* outdir = nullptr;
    for (int i = 2; i + 2 < argc + 0; i++)
        if (!strcmp(argv[i], "--extract") && i + 2 < argc) { prefix = argv[i + 1]; outdir = argv[i + 2]; }

    for (u32 i = 1; i < n; i++) {
        const Entry& en = entries[i];
        if (list && !en.is_dir) printf("%9u  %s\n", en.size, en.path.c_str());
        if (prefix && !en.is_dir && en.path.rfind(prefix, 0) == 0) {
            std::filesystem::path out = std::filesystem::path(outdir) / en.path.substr(1);
            std::filesystem::create_directories(out.parent_path());
            std::vector<u8> buf(en.size);
            if (!vol->Read(en.offset, en.size, buf.data(), part)) {
                fprintf(stderr, "read failed: %s\n", en.path.c_str());
                continue;
            }
            FILE* f = fopen(out.c_str(), "wb");
            fwrite(buf.data(), 1, buf.size(), f);
            fclose(f);
            printf("extracted %s (%u bytes)\n", out.c_str(), en.size);
        }
    }
    return 0;
}
