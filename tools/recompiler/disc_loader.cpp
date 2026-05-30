#include "disc_loader.h"
#include <stdexcept>
#include <cstring>

// Dolphin DiscIO headers — available after `git submodule update --init`
// These are conditionally included so the recompiler compiles without Dolphin
// during early development (falls back to raw ISO/GCM parsing).
#ifdef HAVE_DOLPHIN_DISCIO
#  include "DiscIO/Blob.h"
#  include "DiscIO/Volume.h"
#  include "DiscIO/VolumeGC.h"
#  include "DiscIO/Filesystem.h"
#endif

// ── Fallback: minimal GCM/ISO parser (no RVZ support without Dolphin) ──────

struct GCMHeader {
    char   game_id[6];
    u8     disc_number;
    u8     version;
    u8     streaming;
    u8     stream_buf_size;
    u8     pad[14];
    u32    wii_magic;
    u32    gc_magic;   // 0xC2339F3D
    char   title[64];
    // ...offsets at 0x420
};

static u32 read_be32(const u8* p) {
    return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
}

struct FSTEntry {
    u8  is_dir;
    u32 name_off;  // offset into string table
    u32 offset;    // file offset (files) or parent idx (dirs)
    u32 size;      // file size (files) or next idx (dirs)
    std::string name;
    std::string full_path;
};

struct DiscLoader::Impl {
    std::vector<u8> raw;   // entire disc image in memory (for GCM/ISO)
    bool           ok = false;
    DiscInfo       di;
    u32            dol_offset = 0;
    u32            dol_size   = 0;
    u32            fst_offset = 0;
    u32            fst_size   = 0;
    std::vector<FSTEntry> fst_entries;

#ifdef HAVE_DOLPHIN_DISCIO
    std::unique_ptr<DiscIO::Volume> volume;
#endif

    void parse_gcm(const std::vector<u8>& data) {
        if (data.size() < 0x500)
            throw std::runtime_error("Disc image too small");

        const u8* b = data.data();
        u32 magic = read_be32(b + 0x1C);
        if (magic != 0xC2339F3D)
            throw std::runtime_error("Not a GameCube disc image (wrong magic)");

        di.game_id.assign((char*)b, 6);
        di.title.assign((char*)(b + 0x20), 64);
        // Trim null padding
        di.title = di.title.substr(0, di.title.find('\0'));

        // Boot header at 0x420
        u32 apploader_off = 0x2440;  // standard GCM apploader offset
        dol_offset = read_be32(b + 0x420);
        u32 fst_off = read_be32(b + 0x424);
        fst_size   = read_be32(b + 0x428);
        fst_offset = fst_off;

        // DOL size: we'll infer it from the header
        // (parse DOL header to compute total size)
        if (dol_offset + 0x100 < data.size()) {
            const u8* dh = b + dol_offset;
            u32 max_end = 0;
            for (int i = 0; i < 18; i++) {
                u32 off  = read_be32(dh + (i < 7 ? 0 : 0x1C) + (i < 7 ? i : i-7)*4);
                u32 size = read_be32(dh + (i < 7 ? 0x90 : 0xAC) + (i < 7 ? i : i-7)*4);
                if (off + size > max_end) max_end = off + size;
            }
            dol_size = max_end;
        }

        // Parse FST
        if (fst_off + fst_size <= data.size()) {
            parse_fst(b + fst_off, fst_size, b);
        }

        raw = data;
        ok  = true;
    }

    void parse_fst(const u8* fst, u32 fst_sz, const u8* disc) {
        if (fst_sz < 12) return;
        u32 n_entries = read_be32(fst + 8);
        const u8* str_table = fst + n_entries * 12;

        fst_entries.resize(n_entries);
        for (u32 i = 0; i < n_entries; i++) {
            const u8* e = fst + i * 12;
            FSTEntry& fe = fst_entries[i];
            fe.is_dir   = e[0];
            fe.name_off = ((u32)e[1]<<16)|((u32)e[2]<<8)|e[3];
            fe.offset   = read_be32(e + 4);
            fe.size     = read_be32(e + 8);
            if (fe.name_off < fst_sz - (str_table - fst))
                fe.name = (const char*)(str_table + fe.name_off);
        }

        // Build full paths
        std::vector<std::string> path_stack = {""};
        std::vector<u32> dir_end_stack = {n_entries};
        for (u32 i = 1; i < n_entries; i++) {
            while (path_stack.size() > 1 && i >= dir_end_stack.back()) {
                path_stack.pop_back();
                dir_end_stack.pop_back();
            }
            fst_entries[i].full_path = path_stack.back() + "/" + fst_entries[i].name;
            if (fst_entries[i].is_dir) {
                path_stack.push_back(fst_entries[i].full_path);
                dir_end_stack.push_back(fst_entries[i].size);
            }
        }
    }
};

DiscLoader::DiscLoader(const std::string& path) : impl_(std::make_unique<Impl>()) {
    // Try Dolphin DiscIO first for RVZ/CISO/NFS support
#ifdef HAVE_DOLPHIN_DISCIO
    auto blob = DiscIO::CreateBlobReader(path);
    if (blob) {
        impl_->volume = DiscIO::CreateVolume(path);
        if (impl_->volume) {
            impl_->di.game_id = impl_->volume->GetGameID();
            auto title = impl_->volume->GetLongNames();
            if (!title.empty())
                impl_->di.title = title.begin()->second;
            impl_->ok = true;
            return;
        }
    }
#endif

    // Fallback: load raw file and parse as GCM/ISO
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("Cannot open: " + path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    std::vector<u8> data(sz);
    fread(data.data(), 1, sz, f);
    fclose(f);

    // Check magic — GCM/ISO
    if (data.size() >= 0x20 &&
        ((data[0x1C]==0xC2&&data[0x1D]==0x33&&data[0x1E]==0x9F&&data[0x1F]==0x3D) ||
         (data[0x18]==0x5D&&data[0x19]==0x1C&&data[0x1A]==0x9E&&data[0x1B]==0xA3))) {
        impl_->parse_gcm(data);
    } else {
        throw std::runtime_error(
            "Cannot parse disc image. RVZ files require Dolphin DiscIO.\n"
            "Build with HAVE_DOLPHIN_DISCIO=1 or convert RVZ → ISO first:\n"
            "  DolphinTool convert -i input.rvz -o output.iso -f iso"
        );
    }
}

DiscLoader::~DiscLoader() = default;

bool DiscLoader::is_open() const { return impl_->ok; }

DiscInfo DiscLoader::info() const { return impl_->di; }

std::vector<u8> DiscLoader::load_dol() const {
#ifdef HAVE_DOLPHIN_DISCIO
    if (impl_->volume) {
        // Use Dolphin to extract DOL
        auto* gc = dynamic_cast<DiscIO::VolumeGC*>(impl_->volume.get());
        if (gc) {
            // DOL is at the partition's dol_offset in the volume
            u32 off = 0, sz = 0;
            // Read from boot.bin / bi2.bin to get DOL offset
            std::vector<u8> boot(0x440);
            gc->Read(0, 0x440, boot.data());
            off = (read_be32(boot.data() + 0x420)) << 2;  // Wii shifts, GC doesn't
            // For GC, offset is direct
            off = read_be32(boot.data() + 0x420);
            // Read DOL header to get size
            std::vector<u8> hdr(0x100);
            gc->Read(off, 0x100, hdr.data());
            u32 max_end = 0;
            for (int i = 0; i < 7; i++) {
                u32 o = read_be32(hdr.data() + i*4);
                u32 s = read_be32(hdr.data() + 0x90 + i*4);
                if (o + s > max_end) max_end = o + s;
            }
            for (int i = 0; i < 11; i++) {
                u32 o = read_be32(hdr.data() + 0x1C + i*4);
                u32 s = read_be32(hdr.data() + 0xAC + i*4);
                if (o + s > max_end) max_end = o + s;
            }
            std::vector<u8> dol_data(max_end);
            gc->Read(off, max_end, dol_data.data());
            return dol_data;
        }
    }
#endif

    if (!impl_->ok || impl_->dol_offset == 0) return {};
    u32 off = impl_->dol_offset;
    u32 sz  = impl_->dol_size;
    if (off + sz > impl_->raw.size()) sz = impl_->raw.size() - off;
    return std::vector<u8>(impl_->raw.begin() + off, impl_->raw.begin() + off + sz);
}

std::vector<std::string> DiscLoader::list_files() const {
    std::vector<std::string> result;
    for (const auto& e : impl_->fst_entries) {
        if (!e.is_dir && !e.full_path.empty())
            result.push_back(e.full_path);
    }
    return result;
}

std::vector<u8> DiscLoader::load_file(const std::string& fst_path) const {
    for (const auto& e : impl_->fst_entries) {
        if (e.full_path == fst_path || ("/" + e.name) == fst_path) {
            if (e.is_dir) return {};
            u32 off = e.offset;
            u32 sz  = e.size;
            if (off + sz > impl_->raw.size()) return {};
            return std::vector<u8>(impl_->raw.begin() + off, impl_->raw.begin() + off + sz);
        }
    }
    return {};
}
