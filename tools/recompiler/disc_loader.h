#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

using u8  = uint8_t;
using u32 = uint32_t;

// Thin wrapper around Dolphin's DiscIO to load RVZ/ISO/GCM files.
// Extracts the main DOL, apploader, and FST from a disc image.

struct DiscFile {
    std::string path;
    std::vector<u8> data;
};

struct DiscInfo {
    std::string game_id;   // e.g. "GMSE01"
    std::string title;     // e.g. "Super Mario Sunshine"
    u32         maker_id;
    u32         disc_number;
    u32         version;
};

class DiscLoader {
public:
    explicit DiscLoader(const std::string& path);
    ~DiscLoader();

    // True if the disc image was loaded successfully
    bool is_open() const;

    // Disc metadata
    DiscInfo info() const;

    // Extract the main DOL executable bytes
    std::vector<u8> load_dol() const;

    // List all files on the disc filesystem
    std::vector<std::string> list_files() const;

    // Extract a specific file from the disc by its FST path (e.g. "/scene/mario.rel")
    std::vector<u8> load_file(const std::string& fst_path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
