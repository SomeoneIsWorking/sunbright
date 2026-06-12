// Native JAS audio engine — M1: SE subset (docs/native_audio_engine.md).
//
// PC-owned port of the JAudio sequencer + synth, fed straight from the ROM:
//   data   : nintendo.szs → mSound.aaf (AAF chunk 2 = IBNK list, chunk 3 = WSYS),
//            /AudioRes/Banks/*.aw wave archives, /AudioRes/Seqs/sequence.arc (BMS).
//   seq    : port of JASystem::TSeqParser/TTrack/TSeqCtrl semantics
//            (reference/sms/src/JSystem/JAudio/JASystem) running the REAL init/SE
//            BMS from the ROM (offset 0 of sequence.arc).
//   intake : njas_se_start(id) does exactly what JAIBasic does over track ports:
//            find the idle worker track of category (id>>12)&0xFF (workers export
//            their category on port 9), write port4 = id & 0x3FF, port0 = 1.
//   synth  : IBNK instrument → WSYS wave id → AFC-decoded PCM (decode verified by
//            tools/jingle/jingle.py), pitch via C5BASE table, ADSR via TOscillator
//            port, mixed at the JAS DAC rate and added into the native DSP sink
//            stream at push time (njas_mix called from na_push_dsp).
//
// Subframe clock: everything (track tick, envelope update, gate countdown) runs once
// per 80 output samples at dacRate 32028.5 Hz, matching JAS's subframe callback.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "intrinsics.h"

#include "DiscIO/Blob.h"
#include "DiscIO/Volume.h"

namespace njas {

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t;
using s8 = int8_t;  using s16 = int16_t;  using s32 = int32_t;

static bool dbg() { static int v = -1; if (v < 0) { const char* e = getenv("SUNBRIGHT_DBG_NJAS"); v = (e && *e && *e != '0') ? 1 : 0; } return v; }
#define NJLOG(...) do { if (dbg()) fprintf(stderr, "[njas] " __VA_ARGS__); } while (0)

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
static float bef32(const u8* p) { u32 v = be32(p); float f; memcpy(&f, &v, 4); return f; }

static constexpr float kDacRate = 32028.5f;
static constexpr int   kSub     = 80;     // samples per subframe tick

// ============================ data: ROM loading ============================

struct Wave {
    u8 fmt = 0, key = 60;
    float rate = 32000.f;
    u32 loop = 0, loop_s = 0, loop_e = 0;
    u32 srcHash = 0;                  // FNV-1a of the first 64 raw .aw bytes — the
                                      // oracle-comparison join key (/aram hashes the same
                                      // bytes at the VPB base address in ARAM)
    u16 id = 0; s8 wsysIdx = -1, groupIdx = -1;
    std::vector<s16> pcm;             // fully decoded at load
};
static u32 fnv1a(const u8* p, u32 n) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

struct VeloRegion { u8 maxvel; u16 waveid; float volmul, pitchmul; };
struct KeyRegion  { u8 maxkey; std::vector<VeloRegion> velo; };
struct OscData {
    u8 mode = 0; float rate = 1.f;
    std::vector<s16> attack, release;  // triplet tables (may be empty)
    float width = 1.f, vertex = 0.f;
};
struct InstEffect {                    // rand or sense
    bool is_sense = false; u8 target = 0;
    float rand_base = 1.f, rand_width = 0.f;            // rand
    u8 sense_src = 0, sense_center = 127; float sense_lo = 1.f, sense_hi = 1.f; // sense
};
struct Inst {
    float vol = 1.f, pitch = 1.f;
    std::vector<OscData> oscs;
    std::vector<InstEffect> effects;
    std::vector<KeyRegion> keys;
    // percussion (per-key map), used for insts 0xE4+
    bool is_perc = false;
    struct Perc { bool valid = false; float vol = 1.f, pitch = 1.f, pan = 0.5f; u16 release = 0;
                  std::vector<InstEffect> effects; std::vector<VeloRegion> velo; };
    std::vector<Perc> percs;
};
struct Bank {
    Inst* insts[256] = {};
    int wsys = 0;
};

struct BarcEntry { u32 off = 0, size = 0; char name[15] = {}; };

// A WSYS holds one wave-id table PER SCENE GROUP (wScene has 22 groups whose wave ids
// OVERLAP — different stages reuse the same ids for different samples; a merged table
// plays the wrong waves outside the last-parsed stage). The guest selects the resident
// group per wsys via JAIBasic::loadGroupWave — teed into g_wave_scene[].
struct WsysBank { std::vector<std::vector<Wave>> groups; };

struct Data {
    bool ok = false;
    std::vector<Bank> banks;          // physical index = AAF chunk-2 order
    int vir2phy[256];
    std::vector<WsysBank> wsys_waves; // [wsys].groups[scene][waveid]
    std::vector<u8> seq;              // sequence.arc
    std::vector<BarcEntry> barc;      // AAF chunk 4 "BARC": BGM index → BMS offset in seq
} g_data;

static std::atomic<int> g_wave_scene[8];   // active scene group per wsys (loadGroupWave tee)

// ---- disc access
struct Disc {
    std::unique_ptr<DiscIO::Volume> vol;
    struct Entry { u32 off, size; };
    std::vector<std::pair<std::string, Entry>> files;
    bool open() {
        const char* rom = getenv("SUNBRIGHT_ROM");
        if (!rom || !*rom) { fprintf(stderr, "[njas] SUNBRIGHT_ROM not set — native audio data unavailable\n"); return false; }
        vol = DiscIO::CreateVolume(rom);
        if (!vol) { fprintf(stderr, "[njas] cannot open volume %s\n", rom); return false; }
        u8 boot[0x440];
        if (!vol->Read(0, sizeof(boot), boot, DiscIO::PARTITION_NONE)) return false;
        const u32 fst_off = be32(boot + 0x424), fst_sz = be32(boot + 0x428);
        std::vector<u8> fst(fst_sz);
        if (!vol->Read(fst_off, fst_sz, fst.data(), DiscIO::PARTITION_NONE)) return false;
        const u32 n = be32(fst.data() + 8);
        const u8* strt = fst.data() + n * 12;
        std::vector<std::string> stack = { "" };
        std::vector<u32> dend = { n };
        for (u32 i = 1; i < n; i++) {
            while (stack.size() > 1 && i >= dend.back()) { stack.pop_back(); dend.pop_back(); }
            const u8* e = fst.data() + i * 12;
            const u32 name_off = ((u32)e[1] << 16) | ((u32)e[2] << 8) | e[3];
            std::string path = stack.back() + "/" + (const char*)(strt + name_off);
            if (e[0]) { stack.push_back(path); dend.push_back(be32(e + 8)); }
            else files.push_back({ path, { be32(e + 4), be32(e + 8) } });
        }
        return true;
    }
    bool read(const std::string& path, std::vector<u8>& out) {
        for (auto& f : files)
            if (f.first == path) {
                out.resize(f.second.size);
                return vol->Read(f.second.off, f.second.size, out.data(), DiscIO::PARTITION_NONE);
            }
        fprintf(stderr, "[njas] file not on FST: %s\n", path.c_str());
        return false;
    }
};

// ---- Yaz0 + RARC (mirrors tools/jingle/jingle.py, the verified decoder)
static std::vector<u8> yaz0(const std::vector<u8>& d) {
    std::vector<u8> out;
    if (d.size() < 16 || memcmp(d.data(), "Yaz0", 4)) return out;
    const u32 size = be32(d.data() + 4);
    out.reserve(size);
    size_t i = 16; u8 code = 0; int bits = 0;
    while (out.size() < size && i < d.size()) {
        if (!bits) { code = d[i++]; bits = 8; }
        if (code & 0x80) out.push_back(d[i++]);
        else {
            const u8 b1 = d[i], b2 = d[i + 1]; i += 2;
            size_t src = out.size() - (((b1 & 0xF) << 8) | b2) - 1;
            u32 n = (b1 >> 4) + 2;
            if (n == 2) n = d[i++] + 0x12;
            for (u32 k = 0; k < n; k++) out.push_back(out[src++]);
        }
        code <<= 1; bits--;
    }
    return out;
}
static bool rarc_find(const std::vector<u8>& d, const char* want, std::vector<u8>& out) {
    if (d.size() < 0x40 || memcmp(d.data(), "RARC", 4)) return false;
    const u8* p = d.data();
    const u32 data_off = be32(p + 0xC) + 0x20, node_count = be32(p + 0x20);
    const u32 node_off = be32(p + 0x24) + 0x20, ent_off = be32(p + 0x2C) + 0x20, str_off = be32(p + 0x34) + 0x20;
    for (u32 ni = 0; ni < node_count; ni++) {
        const u8* n = p + node_off + ni * 0x10;
        const u32 count = be16(n + 0xA), first = be32(n + 0xC);
        for (u32 fi = 0; fi < count; fi++) {
            const u8* e = p + ent_off + (first + fi) * 0x14;
            if (be16(e) == 0xFFFF || ((be32(e + 4) >> 24) & 0x02)) continue;
            const char* name = (const char*)(p + str_off + (be32(e + 4) & 0xFFFFFF));
            if (strcmp(name, want)) continue;
            const u32 off = be32(e + 8), size = be32(e + 0xC);
            out.assign(p + data_off + off, p + data_off + off + size);
            return true;
        }
    }
    return false;
}

// ---- AFC decode (verified coefficients, jingle.py / audio_data_formats.md)
static const int kCoef[16][2] = {
    {0,0},{2048,0},{0,2048},{1024,1024},{4096,-2048},{3584,-1536},{3072,-1024},{4608,-2560},
    {4200,-2248},{4800,-2300},{5120,-3072},{2048,-2048},{1024,-1024},{-1024,1024},{-1024,0},{-2048,0},
};
static void afc_decode(const u8* d, u32 len, bool hq, std::vector<s16>& out) {
    int yn1 = 0, yn2 = 0;
    const u32 bs = hq ? 9 : 5;
    out.reserve(out.size() + (len / bs) * 16);
    for (u32 b = 0; b + bs <= len; b += bs) {
        const u8* blk = d + b;
        const int delta = 1 << (blk[0] >> 4);
        const int c0 = kCoef[blk[0] & 0xF][0], c1 = kCoef[blk[0] & 0xF][1];
        for (int i = 0; i < 16; i++) {
            int nib;
            if (hq) { nib = (blk[1 + i / 2] >> (i % 2 ? 0 : 4)) & 0xF; if (nib >= 8) nib -= 16; nib <<= 11; }
            else    { nib = (blk[1 + i / 4] >> (6 - 2 * (i % 4))) & 3; if (nib >= 2) nib -= 4;  nib <<= 13; }
            int s = (delta * nib + yn1 * c0 + yn2 * c1) >> 11;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            yn2 = yn1; yn1 = s;
            out.push_back((s16)s);
        }
    }
}

// ---- IBNK parse (JASBNKParser semantics)
static void parse_osc_table(const u8* base, u32 off, std::vector<s16>& out) {
    if (!off) return;
    const u8* p = base + off;
    for (;;) {
        s16 mode = (s16)be16(p), time = (s16)be16(p + 2), value = (s16)be16(p + 4);
        out.push_back(mode); out.push_back(time); out.push_back(value);
        p += 6;
        if (mode > 0xa) break;
    }
}
static Inst* parse_inst(const u8* bnk, u32 off) {
    if (!off) return nullptr;
    const u8* p = bnk + off;
    Inst* in = new Inst();
    in->vol = bef32(p + 0x8);
    in->pitch = bef32(p + 0xC);
    for (int j = 0; j < 2; j++) {
        const u32 osc_off = be32(p + 0x10 + j * 4);
        if (!osc_off) continue;
        const u8* o = bnk + osc_off;
        OscData od;
        od.mode = o[0];
        od.rate = bef32(o + 4);
        parse_osc_table(bnk, be32(o + 8), od.attack);
        parse_osc_table(bnk, be32(o + 0xC), od.release);
        od.width = bef32(o + 0x10);
        od.vertex = bef32(o + 0x14);
        in->oscs.push_back(std::move(od));
    }
    for (int j = 0; j < 2; j++) {       // rand
        const u32 ro = be32(p + 0x18 + j * 4);
        if (!ro) continue;
        const u8* r = bnk + ro;
        InstEffect ef; ef.is_sense = false; ef.target = r[0];
        ef.rand_base = bef32(r + 4); ef.rand_width = bef32(r + 8);
        in->effects.push_back(ef);
    }
    for (int j = 0; j < 2; j++) {       // sense
        const u32 so = be32(p + 0x20 + j * 4);
        if (!so) continue;
        const u8* s = bnk + so;
        InstEffect ef; ef.is_sense = true; ef.target = s[0];
        ef.sense_src = s[1]; ef.sense_center = s[2];
        ef.sense_lo = bef32(s + 4); ef.sense_hi = bef32(s + 8);
        in->effects.push_back(ef);
    }
    const u32 kcount = be32(p + 0x28);
    for (u32 j = 0; j < kcount; j++) {
        const u32 ko = be32(p + 0x2C + j * 4);
        if (!ko) continue;
        const u8* k = bnk + ko;
        KeyRegion kr; kr.maxkey = k[0];
        const u32 vcount = be32(k + 4);
        for (u32 v = 0; v < vcount; v++) {
            const u32 vo = be32(k + 8 + v * 4);
            if (!vo) continue;
            const u8* vm = bnk + vo;
            VeloRegion vr;
            vr.maxvel = vm[0];
            vr.waveid = (u16)(be32(vm + 4) & 0xFFFF);
            vr.volmul = bef32(vm + 8);
            vr.pitchmul = bef32(vm + 0xC);
            kr.velo.push_back(vr);
        }
        in->keys.push_back(std::move(kr));
    }
    return in;
}
static void parse_ibnk(const u8* bnk, u32 size, Bank& out) {
    (void)size;
    for (int i = 0; i < 0x80; i++)
        out.insts[i] = parse_inst(bnk, be32(bnk + 0x24 + i * 4));
    for (int i = 0; i < 12; i++) {       // percussion sets → insts 0xE4+
        const u32 po = be32(bnk + 0x3B4 + i * 4);
        if (!po) continue;
        const u8* perc = bnk + po;
        Inst* in = new Inst();
        in->is_perc = true;
        in->percs.resize(0x80);
        const bool per2 = !memcmp(perc, "PER2", 4);
        for (int j = 0; j < 0x80; j++) {
            const u32 pm = be32(perc + 0x88 + j * 4);
            if (!pm) continue;
            const u8* pmap = bnk + pm;
            auto& pc = in->percs[j];
            pc.valid = true;
            // TPmap layout is PITCH first, then volume (pikmin2 JASBNKParser TPmap:
            // mPitch @+0, mVolume @+4 — reverse of TInst's vol@8/pitch@C order!).
            // Reading them swapped made drums quiet-and-sharp (the 0:233 −30 dB /
            // +105-cent A/B signature).
            pc.pitch = bef32(pmap);
            pc.vol = bef32(pmap + 4);
            // PER2 extras (JASBNKParser): unk288[j]/127 is the per-key PAN (the old code
            // multiplied it into volume — percussion loudness bug), unk308[j] = release.
            if (per2) { pc.pan = (float)perc[0x288 + j] / 127.f; pc.release = be16(perc + 0x308 + j * 2); }
            for (int e = 0; e < 2; e++) {              // pmap rand effects (TRand only)
                const u32 ro = be32(pmap + 0x8 + e * 4);
                if (!ro) continue;
                const u8* r = bnk + ro;
                InstEffect ef; ef.is_sense = false; ef.target = r[0];
                ef.rand_base = bef32(r + 4); ef.rand_width = bef32(r + 8);
                pc.effects.push_back(ef);
            }
            const u32 vcount = be32(pmap + 0x10);
            for (u32 v = 0; v < vcount; v++) {
                const u32 vo = be32(pmap + 0x14 + v * 4);
                if (!vo) continue;
                const u8* vm = bnk + vo;
                VeloRegion vr;
                vr.maxvel = vm[0];
                vr.waveid = (u16)(be32(vm + 4) & 0xFFFF);
                vr.volmul = bef32(vm + 8);
                vr.pitchmul = bef32(vm + 0xC);
                pc.velo.push_back(vr);
            }
        }
        out.insts[0xE4 + i] = in;
    }
}

// ---- WSYS parse (JASWSParser semantics: ctrl wave-id table + WINF archive entries).
// Each scene group gets its OWN table — ids overlap between groups (different content).
static void parse_wsys(const u8* d, u32 size, Disc& disc, WsysBank& bank) {
    (void)size;
    if (memcmp(d, "WSYS", 4)) return;
    const u32 winf = be32(d + 0x10);          // archive bank ("WINF")
    const u32 wbct = be32(d + 0x14);          // ctrl group ("WBCT")
    if (memcmp(d + winf, "WINF", 4)) { fprintf(stderr, "[njas] WSYS: no WINF\n"); return; }
    const u32 group_count = be32(d + wbct + 8);
    const u32 arc_count = be32(d + winf + 4);
    bank.groups.resize(std::min(group_count, arc_count));
    for (u32 g = 0; g < group_count && g < arc_count; g++) {
        std::vector<Wave>& table = bank.groups[g];
        const u32 scene_off = be32(d + wbct + 0xC + g * 4);
        const u32 ctrl_off = be32(d + scene_off + 0xC);
        const u32 wave_count = be32(d + ctrl_off + 4);
        u32 max_id = 0;
        for (u32 w = 0; w < wave_count; w++) {
            const u32 cw = be32(d + ctrl_off + 8 + w * 4);
            const u32 id = be32(d + cw) & 0xFFFF;
            if (id > max_id) max_id = id;
        }
        table.resize(max_id + 1);
        const u32 arc_off = be32(d + winf + 8 + g * 4);
        const char* aw_name = (const char*)(d + arc_off);
        std::vector<u8> aw;
        if (!disc.read(std::string("/AudioRes/Banks/") + aw_name, aw)) continue;
        const u32 wave_count_a = be32(d + arc_off + 0x70);
        for (u32 w = 0; w < wave_count && w < wave_count_a; w++) {
            const u32 we = be32(d + arc_off + 0x74 + w * 4);
            const u32 cw = be32(d + ctrl_off + 8 + w * 4);
            const u32 id = be32(d + cw) & 0xFFFF;
            Wave& wv = table[id];
            wv.fmt = d[we + 1];
            wv.key = d[we + 2];
            wv.rate = bef32(d + we + 4);
            wv.id = (u16)id;
            wv.groupIdx = (s8)g;
            const u32 start = be32(d + we + 8), len = be32(d + we + 0xC);
            if (start + 64 <= aw.size()) wv.srcHash = fnv1a(aw.data() + start, len < 64 ? len : 64);
            wv.loop = be32(d + we + 0x10);
            wv.loop_s = be32(d + we + 0x14);
            wv.loop_e = be32(d + we + 0x18);
            wv.pcm.clear();
            if (start + len <= aw.size()) {
                if (wv.fmt <= 1) afc_decode(aw.data() + start, len, wv.fmt == 0, wv.pcm);
                else if (wv.fmt == 3) { // PCM16
                    wv.pcm.resize(len / 2);
                    for (u32 i = 0; i < len / 2; i++) wv.pcm[i] = (s16)be16(aw.data() + start + i * 2);
                } else if (wv.fmt == 2) { // PCM8
                    wv.pcm.resize(len);
                    for (u32 i = 0; i < len; i++) wv.pcm[i] = (s16)((s8)aw[start + i] << 8);
                }
            }
        }
        NJLOG("wsys group %u: %s (%u waves)\n", g, aw_name, wave_count);
    }
}

static bool load_data() {
    Disc disc;
    if (!disc.open()) return false;
    std::vector<u8> szs;
    if (!disc.read("/data/nintendo.szs", szs)) return false;
    std::vector<u8> rarc = yaz0(szs);
    std::vector<u8> aaf;
    if (!rarc_find(rarc, "msound.aaf", aaf)) { fprintf(stderr, "[njas] msound.aaf not in nintendo.szs\n"); return false; }
    if (!disc.read("/AudioRes/Seqs/sequence.arc", g_data.seq)) return false;

    for (int i = 0; i < 256; i++) g_data.vir2phy[i] = -1;

    // AAF chunk walk: ids 1/4/5/6/7 single (offset,size,flag); 2 = IBNK list; 3 = WSYS list
    u32 o = 0;
    std::vector<std::pair<u32, u32>> ibnks;   // offset, wsys-assignment flag
    std::vector<u32> wsyss;
    u32 barc_off = 0;
    while (o + 4 <= aaf.size()) {
        const u32 cid = be32(aaf.data() + o); o += 4;
        if (cid == 0) break;
        if (cid == 4) barc_off = be32(aaf.data() + o);   // BARC sequence collection
        if (cid == 1 || (cid >= 4 && cid <= 7)) { o += 12; continue; }
        if (cid == 2 || cid == 3) {
            for (;;) {
                const u32 off = be32(aaf.data() + o);
                if (!off) { o += 4; break; }
                const u32 flag = be32(aaf.data() + o + 8);
                o += 12;
                if (cid == 2) ibnks.push_back({ off, flag });
                else wsyss.push_back(off);
            }
            continue;
        }
        fprintf(stderr, "[njas] unknown AAF chunk %u\n", cid);
        return false;
    }

    g_data.wsys_waves.resize(wsyss.size());
    for (size_t i = 0; i < wsyss.size(); i++) {
        parse_wsys(aaf.data() + wsyss[i], 0, disc, g_data.wsys_waves[i]);
        for (auto& grp : g_data.wsys_waves[i].groups)
            for (auto& w : grp) w.wsysIdx = (s8)i;
    }

    g_data.banks.resize(ibnks.size());
    for (size_t i = 0; i < ibnks.size(); i++) {
        const u8* bnk = aaf.data() + ibnks[i].first;
        const u32 virt = be32(bnk + 8);
        if (virt < 256) g_data.vir2phy[virt] = (int)i;
        g_data.banks[i].wsys = (int)ibnks[i].second;
        parse_ibnk(bnk, 0, g_data.banks[i]);
        NJLOG("bank phys %zu: virt %u wsys %d\n", i, virt, g_data.banks[i].wsys);
    }
    if (getenv("SUNBRIGHT_DUMP_IBNK")) {       // offline inspection of parsed bank data
        FILE* f = fopen("scratch/logs/ibnk_dump.txt", "w");
        for (size_t b = 0; f && b < g_data.banks.size(); b++)
            for (int p = 0; p < 0x100; p++) {
                const Inst* in = g_data.banks[b].insts[p];
                if (!in) continue;
                if (!in->is_perc) {
                    fprintf(f, "bank%zu prog%d inst vol=%.4f pitch=%.4f oscs=%zu effs=%zu\n",
                            b, p, in->vol, in->pitch, in->oscs.size(), in->effects.size());
                    continue;
                }
                for (int k = 0; k < 0x80; k++) {
                    const auto& pc = in->percs[k];
                    if (!pc.valid) continue;
                    fprintf(f, "bank%zu prog%d key%d vol=%.4f pitch=%.4f pan=%.3f rel=%u",
                            b, p, k, pc.vol, pc.pitch, pc.pan, pc.release);
                    for (auto& vr : pc.velo)
                        fprintf(f, " [maxvel=%u wave=%u volmul=%.4f pitchmul=%.4f]",
                                vr.maxvel, vr.waveid, vr.volmul, vr.pitchmul);
                    fprintf(f, "\n");
                }
            }
        for (size_t ws = 0; f && ws < g_data.wsys_waves.size(); ws++)
            for (size_t g = 0; g < g_data.wsys_waves[ws].groups.size(); g++)
                for (const auto& w : g_data.wsys_waves[ws].groups[g])
                    fprintf(f, "wsys%zu grp%zu wave%u fmt=%u key=%u rate=%.0f loop=%u "
                               "ls=%u le=%u pcm=%zu\n",
                            ws, g, w.id, w.fmt, w.key, w.rate, w.loop, w.loop_s, w.loop_e,
                            w.pcm.size());
        if (f) fclose(f);
    }
    // BARC: "BARC----" magic, count @+0xC, archive name @+0x10, 0x20-byte entries @+0x20:
    // name[14], u16 flags, u32×2, u32 offset (into sequence.arc), u32 size
    if (barc_off && !memcmp(aaf.data() + barc_off, "BARC", 4)) {
        const u8* b = aaf.data() + barc_off;
        const u32 n = be32(b + 0xC);
        g_data.barc.resize(n);
        for (u32 i = 0; i < n; i++) {
            const u8* e = b + 0x20 + i * 0x20;
            memcpy(g_data.barc[i].name, e, 14);
            g_data.barc[i].off = be32(e + 0x18);
            g_data.barc[i].size = be32(e + 0x1C);
        }
        NJLOG("BARC: %u sequences (1=%s 16=%s)\n", n,
              n > 1 ? g_data.barc[1].name : "?", n > 16 ? g_data.barc[16].name : "?");
    } else fprintf(stderr, "[njas] no BARC chunk — BGM unavailable\n");
    NJLOG("data loaded: %zu banks, %zu wsys, seq %zu bytes\n",
          g_data.banks.size(), g_data.wsys_waves.size(), g_data.seq.size());
    g_data.ok = true;
    return true;
}

// ============================ sequencer + synth ============================

// A/B event stream (defined in the driver section below)
static FILE* ab_out();
static double ab_now_ms();
static void ab_anchor(const char* kind, u32 id, const char* name);

// C5BASE pitch table (JASDriverTables): 2^((k-60)/12)
static float c5base(int k) {
    if (k < 0) k = 0; if (k > 127) k = 127;
    return exp2f((k - 60) / 12.0f);
}

// --- TOscillator port (JASOscillator.cpp)
struct Oscillator {
    const OscData* osc = nullptr;
    u8 state = 1, curve = 0;
    int idx = 0;
    float relRate = 0, phase = 0, targetPhase = 0, phaseChange = 0, initialRel = 0;
    u16 directRelease = 0;

    void init() { state = 1; curve = 0; idx = 0; relRate = phase = targetPhase = phaseChange = initialRel = 0; directRelease = 0; }
    void set(const OscData* o) { osc = o; }
    bool isOsc() const { return osc != nullptr; }
    void initStart() {
        state = 2; directRelease = 0;
        if (!osc || osc->attack.empty()) { phase = 0; return; }
        idx = 0; relRate = 0; targetPhase = 0;
        relRate -= osc->rate;
    }
    static const s16* force_stop_table() { static const s16 t[6] = { 0, 15, 0, 15, 0, 0 }; return t; }
    bool release() {
        if (state == 5) return false;
        if (!osc) return false;
        if (osc->attack.data() != osc->release.data()) { idx = 0; relRate = 0; targetPhase = phase; }
        if (osc->release.empty() && directRelease == 0) directRelease = 0x10;
        if (directRelease) {
            state = 6;
            curve = (directRelease >> 14) & 3;
            float t = (float)(directRelease & 0x3FFF);
            t *= (kDacRate / 80.f) / 600.f;   // updateInterval == 1
            relRate = t < 1.f ? 1.f : t;
            initialRel = relRate;
            targetPhase = 0;
            phaseChange = curve == 0 ? (targetPhase - phase) / relRate : targetPhase - phase;
        } else state = 4;
        return true;
    }
    bool forceStop() {
        if (state == 5) return false;
        idx = 0; relRate = 0; targetPhase = phase; state = 5;
        return true;
    }
    void releaseDirect(u16 r) { directRelease = r; }
    float getOffset() {
        if (!osc) { phase = 1.f; return 1.f; }
        switch (state) {
        case 0: return 1.f;
        case 3: return osc->vertex + phase * osc->width;
        case 1: state = 2; [[fallthrough]];
        default: {
            const s16* tbl;
            int tbl_n;
            if (state == 4)      { tbl = osc->release.data(); tbl_n = (int)osc->release.size(); }
            else if (state == 5) { tbl = force_stop_table(); tbl_n = 6; }
            else                 { tbl = osc->attack.data();  tbl_n = (int)osc->attack.size(); }
            if ((!tbl || !tbl_n) && state != 6) { phase = 1.f; return 1.f; }
            relRate -= (state == 5) ? 1.f : osc->rate;
            return calc(tbl, tbl_n);
        }
        }
    }
    float calc(const s16* table, int tbl_n) {
        static const float relCell[17] = { 1.f,0.970489f,0.781274f,0.546281f,0.399792f,0.289315f,0.212104f,0.157476f,0.112613f,0.0817896f,0.0579852f,0.0436415f,0.0308237f,0.0237129f,0.0152593f,0.00915555f,0.f };
        static const float relSqRoot[17] = { 1.f,0.878906f,0.765625f,0.660156f,0.5625f,0.472656f,0.390625f,0.316406f,0.25f,0.191406f,0.140625f,0.0976562f,0.0625f,0.0351562f,0.015625f,0.00390625f,0.f };
        static const float relSquare[17] = { 1.f,0.968246f,0.935414f,0.901388f,0.866025f,0.829156f,0.790569f,0.75f,0.707107f,0.661438f,0.612372f,0.559017f,0.5f,0.433013f,0.353553f,0.25f,0.f };
        while (relRate <= 0.f) {
            phase = targetPhase;
            if (state == 6) { state = 0; break; }
            if (!table || idx * 3 + 2 >= tbl_n) { state = 0; break; }
            const s16 mode = table[idx * 3], time = table[idx * 3 + 1], value = table[idx * 3 + 2];
            if (mode == 13) { idx = value; continue; }
            if (mode == 15) { state = 0; break; }
            if (mode == 14) { state = 3; return osc->vertex + phase * osc->width; }
            curve = (u8)mode;
            if (time == 0) { targetPhase = value / 32768.f; idx++; continue; }
            relRate = time * ((kDacRate / 80.f) / 600.f);
            initialRel = relRate;
            targetPhase = value / 32768.f;
            phaseChange = curve == 0 ? (targetPhase - phase) / relRate : targetPhase - phase;
            idx++;
        }
        if (!osc->width) return osc->vertex;
        float np;
        if (initialRel == 0.f) { np = targetPhase; phase = np; }
        else if (curve == 0 || phaseChange == 0.f) { np = targetPhase - phaseChange * relRate; phase = np; }
        else if (curve == 3 || curve == 1 || curve == 2) {
            const float* tb = curve == 3 ? relCell : curve == 1 ? relSquare : relSqRoot;
            float fidx = phaseChange < 0.f ? 16.f * (1.f - relRate / initialRel) : 16.f * (relRate / initialRel);
            u32 i = (u32)fidx;
            float prop = fidx - i;
            if (i >= 16) { i = 15; prop = 1.f; }
            float vabs = fabsf(phaseChange * (prop * (tb[i + 1] - tb[i]) + tb[i]));
            np = phaseChange < 0.f ? targetPhase + vabs : targetPhase - (phaseChange - vabs);
            phase = np;
        } else { np = targetPhase - phaseChange * relRate; phase = np; }
        return np * osc->width + osc->vertex;
    }
};

// --- voice (TChannel port, with direct PCM render instead of DSP buffers)
struct Track;
struct Voice {
    bool active = false;
    u8 status = 0xFF;          // 0xFF = free/done (TChannel::unk1)
    u32 gen = 0;               // generation (unkC6)
    Track* owner = nullptr;
    const Wave* wave = nullptr;
    double srcpos = 0;
    float baseRatio = 1.f;     // unk48: instpitch * waverate/dacrate
    float pitchRatio = 1.f;    // unk50: * key pitch
    float volBase = 1.f;       // unk4C
    float vol = 1.f;           // unk54: volBase * (vel/127)^2 * effects
    float oscPitch = 1.f, oscVol = 1.f;       // unk8C / unk90
    float panSound = 0.5f, panEffect = 0.5f;  // unk68
    float fxSound = 0.f, dolbySound = 0.f;
    s32 gate = -1, gateLeft = -1;              // unk30 / unk34 (update ticks)
    u32 age = 0;                               // subframes since start (diagnostics)
    u8 key = 0, vel = 0; u16 bankProg = 0;     // noteOn metadata (probe/compare)
    float lastRatio = 0.f, lastVol = 0.f;      // effective values from the last subframe
    float lastChVol = 0.f, lastOscVol = 0.f;   // chain factors (probe diagnosis)
    bool abOpen = false; float abPeak = 0.f;   // A/B event stream bookkeeping
    u32 abSeqOff = 0, abSoundId = 0;
    bool pause = false;
    Oscillator oscs[2];
    OscData trackOscCopy[2];   // storage when track overwrites osc
    // sweep
    float sweepTarget = 0; s32 sweepLeft = 0;

    void stop_now() { active = false; status = 0xFF; }
};

static constexpr int kMaxVoices = 64;
static Voice g_voices[kMaxVoices];
static u32 g_voice_gen = 1;

// --- TSeqCtrl port
struct SeqCtrl {
    const u8* base = nullptr;
    const u8* cur = nullptr;
    s32 waitTimer = 0;
    int loopIdx = 0;
    const u8* loopStart[8] = {};
    u16 loopTimer[8] = {};
    const u8* intrRet = nullptr;
    s32 intrWaitSave = 0;

    void init(const u8* data, u32 off) {
        base = data; cur = data + off; waitTimer = 0; loopIdx = 0;
        for (int i = 0; i < 8; i++) { loopStart[i] = nullptr; loopTimer[i] = 0; }
        intrRet = nullptr; intrWaitSave = 0;
    }
    u8 readByte() { return *cur++; }
    u16 read16() { u16 v = be16(cur); cur += 2; return v; }
    u32 read24() { u32 v = ((u32)cur[0] << 16) | ((u32)cur[1] << 8) | cur[2]; cur += 3; return v; }
    u8 getByte(u32 o) const { return base[o]; }
    u16 get16(u32 o) const { return be16(base + o); }
    u32 get24(u32 o) const { return ((u32)base[o] << 16) | ((u32)base[o + 1] << 8) | base[o + 2]; }
    u32 get32(u32 o) const { return be32(base + o); }
    void jump(u32 off) { cur = base + off; }
    void call(u32 off) { if (loopIdx < 8) { loopStart[loopIdx] = cur; loopTimer[loopIdx] = 0; loopIdx++; } cur = base + off; }
    bool ret() { if (!loopIdx) return false; loopIdx--; cur = loopStart[loopIdx]; return true; }
    void loopS(u16 n) { if (loopIdx < 8) { loopStart[loopIdx] = cur; loopTimer[loopIdx] = n; loopIdx++; } }
    bool loopE() {
        if (!loopIdx) return false;
        u16 t = loopTimer[loopIdx - 1];
        if (t) t--;
        if (!t) { loopIdx--; return true; }
        loopTimer[loopIdx - 1] = t;
        cur = loopStart[loopIdx - 1];
        return true;
    }
    void wait(s32 t) { waitTimer = t; }
    bool waitCountDown() { if (waitTimer > 0) { waitTimer--; if (waitTimer) return false; } return true; }
    bool callIntr(const u8* h) { if (intrRet) return false; intrRet = cur; cur = h; intrWaitSave = waitTimer; waitTimer = 0; return true; }
    bool retIntr() { if (!intrRet) return false; waitTimer = intrWaitSave; cur = intrRet; intrRet = nullptr; return true; }
};

struct IntrMgr {
    u8 enabled = 1; u32 pending = 0, mask = 0;
    u32 handler[8] = {};
    u16 timer = 0, timerCount = 0, timerPeriod = 0;
    void init() { enabled = 1; pending = mask = 0; for (auto& h : handler) h = 0; timer = timerCount = timerPeriod = 0; }
    void request(u32 i) { if (mask & (1u << i)) pending |= 1u << i; }
    void setIntr(u32 i, u32 off) { mask |= 1u << i; handler[i] = off; }
    void resetIntr(u32 i) { mask &= ~(1u << i); }
    s32 check() {
        if (!enabled) return -1;
        u32 r = mask & pending;
        for (u32 i = 0; r; i++, r >>= 1)
            if (r & 1) { pending &= ~(1u << i); return (s32)handler[i]; }
        return -1;
    }
    void timerProcess() {
        if (!timer) return;
        if (--timer) return;
        request(6);
        if (timerCount) { if (--timerCount) timer = timerPeriod; }
        else timer = timerPeriod;
    }
};

struct MoveParam { float cur = 1.f, target = 1.f, amount = 0.f, time = 0.f; };

struct Track {
    bool used = false, opened = false;
    Track* parent = nullptr;
    Track* child[16] = {};
    SeqCtrl seq;
    IntrMgr intr;
    // note mgr
    Voice* note[8] = {};
    u32 noteGen[8] = {};
    u32 baseTime = 0; u8 connectCase = 0, lastNote = 0; bool beforeTie = false;
    // registers
    u16 reg[48] = {};
    u16 bankProg = 0xF0 << 8 | 0;  // unkC: bank/prog (init 0xf0 / 0x0c? see init())
    u16 pitchRange = 12;           // unkE
    s16 panPower[5] = { 0, 1, 1, 0x7FFF, 0x4000 };
    // timed params: 0=vol 1=pitch 2=fxmix 3=pan 4=dolby
    MoveParam timed[18];
    float tickPerCall = 0, tickAcc = 0;
    u16 tempo = 120, timebase = 120;
    u8 tempoChild = 0;             // unk3BD
    s8 transposeSelf = 0, transposeTotal = 0;
    u8 pauseStatus = 0, mute = 0, volumeMode = 0, timeRelate = 0, paused = 0;
    u8 trackMode = 0;              // unk3BC (param3 of opentrack)
    u8 active = 0;                 // unk3C4
    OscData oscSrc[2];             // unk30C
    Oscillator selfOsc[2];         // unk33C (route 0xE)
    u8 oscRoute[2] = { 0xF, 0xE };
    s16 adsTable[12]; s16 relTable[6];
    // track ports
    u16 portValue[16] = {};
    u8 portImport[16] = {}, portExport[16] = {};
    // TOuterParam port (JASTrack.cpp:391-405) — JAI-side volume/pitch/pan on SE workers
    float outerVol = 1.f, outerPitch = 1.f, outerPan = 0.5f;
    u16 outerSwitch = 0;   // 1=vol 2=pitch 8=pan
    // effective channel params (TChannelMgr mVolume etc.)
    float chVol = 1.f, chPitch = 1.f, chPan = 0.5f, chFx = 0.f, chDolby = 0.f;
    u32 updateFlags = 0;           // unk3B4

    void writePortImport(u32 port, u16 v) {
        portImport[port & 15] = 1;
        portValue[port & 15] = v;
        if ((port & 15) <= 1) intr.request((port & 15) == 0 ? 3 : 4);
    }
};

static Track g_tracks[64];
static Track* g_root = nullptr;

static Track* track_alloc() {
    for (auto& t : g_tracks)
        if (!t.used) { t = Track(); t.used = true; return &t; }
    fprintf(stderr, "[njas] out of tracks\n");
    return nullptr;
}

// defaults (JASPlayer_impl)
static const s16 kAdsDef[12] = { 0,0,0x7fff, 0,0,0x7fff, 0,0,0, 0xe,0,0 };
static const s16 kRelDef[6] = { 0,0xa,0, 0xf,1,0 };
static const s16 kVibTable[18] = { 0,0,0, 0,0xC,0x7FFF, 0,0xC,0, 0,0xC,(s16)0xC000, 0,0xC,0, 0xD,0,1 };
static OscData kEnvelopeDef, kVibratoDef;
static void init_osc_defaults() {
    kEnvelopeDef.mode = 0; kEnvelopeDef.rate = 1.f; kEnvelopeDef.width = 1.f; kEnvelopeDef.vertex = 0.f;
    kEnvelopeDef.release.assign(kRelDef, kRelDef + 6);
    kVibratoDef.mode = 1; kVibratoDef.rate = 0.5f; kVibratoDef.width = 0.f; kVibratoDef.vertex = 1.f;
    kVibratoDef.attack.assign(kVibTable, kVibTable + 18);
    kVibratoDef.release.assign(kVibTable, kVibTable + 18);
}

static void track_init_timed(Track& t) {
    for (int i = 0; i < 18; i++) { t.timed[i] = MoveParam(); }
    t.timed[1].cur = t.timed[1].target = 0.f;             // pitch
    t.timed[3].cur = t.timed[3].target = 0.5f;            // pan
    t.timed[16].cur = t.timed[16].target = 0.5f;
    t.timed[17].cur = t.timed[17].target = 0.f;
    t.timed[2].cur = t.timed[2].target = 0.f;             // fxmix
    t.timed[4].cur = t.timed[4].target = 0.f;             // dolby
    for (int i = 13; i < 16; i++) { t.timed[i].cur = t.timed[i].target = 0.f; }
    t.timed[5].cur = t.timed[5].target = 0.f;
}

static void track_update(Track& t);

static void track_init(Track& t, const u8* data, u32 off, Track* parent) {
    t.seq.init(data, off);
    if (!parent) {
        t.tempo = 0x78; t.timebase = 0x30; t.timeRelate = 1; t.paused = 0;
        t.pauseStatus = 10; t.volumeMode = 0; t.mute = 0;
    } else {
        t.tempo = parent->tempo; t.tempoChild = 0; t.tickPerCall = parent->tickPerCall;
        t.timebase = parent->timebase; t.timeRelate = parent->timeRelate;
        t.paused = parent->paused; t.pauseStatus = parent->pauseStatus;
        t.volumeMode = parent->volumeMode; t.mute = parent->mute;
    }
    for (int i = 0; i < 8; i++) { t.note[i] = nullptr; t.noteGen[i] = 0; }
    t.baseTime = 0; t.connectCase = 0; t.lastNote = 0; t.beforeTie = false;
    t.active = 1;
    t.parent = parent;
    t.intr.init();
    track_init_timed(t);
    if ((t.trackMode & 2) || !parent) {
        memset(t.reg, 0, sizeof(t.reg));
        t.bankProg = 0x00F0;     // init(): unkC = 0xf0 (bank 0, prog 0xf0)
        t.pitchRange = 0x0c;
        t.panPower[0] = 0; t.panPower[1] = 1; t.panPower[2] = 1; t.panPower[3] = 0x7FFF; t.panPower[4] = 0x4000;
    } else {
        memset(t.reg, 0, sizeof(t.reg));
        t.bankProg = parent->bankProg;
        t.pitchRange = parent->pitchRange;
        memcpy(t.panPower, parent->panPower, sizeof(t.panPower));
    }
    for (int i = 0; i < 16; i++) t.child[i] = nullptr;
    t.oscSrc[0] = kEnvelopeDef;
    t.oscRoute[0] = 0xF;
    t.oscSrc[1] = kVibratoDef;
    t.selfOsc[1].set(&t.oscSrc[1]);
    t.selfOsc[1].initStart();
    t.oscRoute[1] = 0xE;
    t.transposeSelf = 0; t.transposeTotal = 0;
    memset(t.portValue, 0, sizeof(t.portValue));
    memset(t.portImport, 0, sizeof(t.portImport));
    memset(t.portExport, 0, sizeof(t.portExport));
    memcpy(t.adsTable, kAdsDef, sizeof(t.adsTable));
    memcpy(t.relTable, kRelDef, sizeof(t.relTable));
}

static void track_update_tempo(Track& t) {
    if (!t.parent) {
        float v = (float)t.timebase * t.tempo;
        v /= kDacRate;
        v *= 80.f / 60.f;
        t.tickPerCall = v;
    } else {
        t.tickPerCall = t.parent->tickPerCall;
        t.timebase = t.parent->timebase;
    }
    for (int i = 0; i < 16; i++)
        if (t.child[i] && t.child[i]->active) track_update_tempo(*t.child[i]);
}

// register access (TTrack::readRegDirect / writeRegDirect / readReg32 / exchangeRegisterValue)
static u16 reg_read(Track& t, u8 r) {
    switch (r) {
    case 32: return (u16)(t.bankProg >> 8);
    case 33: return (u16)(t.bankProg & 0xFF);
    case 34: return (u16)((reg_read(t, 0) << 8) | (reg_read(t, 1) & 0xFF));
    case 44: {
        u16 v = 0;
        for (int i = 15; i >= 0; i--) { v <<= 1; if (t.child[i] && t.child[i]->active) v |= 1; }
        return v;
    }
    case 45: {
        u16 v = 0;
        for (int i = 7; i >= 0; i--) {
            v <<= 1;
            Voice* c = t.note[i];
            if (!c || !c->active || c->gen != t.noteGen[i] || c->status == 0xFF) v |= 1;
        }
        return v;
    }
    case 48: return t.seq.loopIdx == 0 ? 0 : t.seq.loopTimer[t.seq.loopIdx - 1];
    default: return r < 48 ? t.reg[r] : 0;
    }
}
static u16 extend8to16(u8 v) { return (v & 0x80) ? (u16)(v + 0xFF00) : v; }
static void reg_write(Track& t, u8 r, u16 v) {
    u16 r4;
    switch (r) {
    case 0: case 1: case 2: v &= 0xff; r4 = extend8to16((u8)v); break;
    case 32: case 33: return;
    case 34: {
        u16 top = v >> 8;
        t.reg[0] = top; t.reg[3] = extend8to16((u8)top);
        r4 = v; v &= 0xff; r = 1;
        break;
    }
    case 6: t.bankProg = v; r4 = v; break;   // keep bank/prog mirrored
    default: r4 = v; break;
    }
    if (r < 48) t.reg[r] = v;
    t.reg[3] = r4;
}
static u32 reg_read32(Track& t, u8 r) {
    if (r >= 0x28 && r <= 0x2B) return 0;  // unk20[] table bases (BGM tables; M2)
    if (r == 0x23) return ((u32)reg_read(t, 4) << 16) | reg_read(t, 5);
    return reg_read(t, r);
}
static u32 reg_exchange(Track& t, u8 r) {
    if (r < 0x40) return reg_read32(t, r);
    int i = r - 0x40;
    return i < 8 ? t.noteGen[i] : 0;
}

static float pitch_to_cent(float pitch, float range) {
    float v = pitch * 4.f * range;
    s16 whole = (s16)v;
    float frac = v - whole;
    if (v < 0.f && frac != 0.f) { frac += 1.f; whole -= 1; }
    if (frac >= 1.f) { frac -= 1.f; whole += 1; }
    // s_key_table[frac*64] ≈ 2^(frac/(12*4*64/48))... use exact: key_table[i] = 2^(i/768)
    return exp2f(frac / 12.f / 4.f * 4.f / 16.f * 16.f / 4.f) /*== 2^(frac/12*?)*/;
}

// NOTE: pitchToCent semantics: result = key_table[frac*64] * C5BASE[whole+60]
// where key_table[i] = 2^(i/768) (64 steps per semitone-quarter…). We compute exactly:
static float pitch_to_ratio(float pitch, float range) {
    float v = pitch * 4.f * range;            // quarter-semitone steps? (4 steps/semitone)
    return exp2f(v / 48.f);                   // 48 steps per octave: C5BASE[whole+60]*2^(frac/48)
}

static Voice* track_voice(Track& t, int idx) {
    Voice* c = t.note[idx & 7];
    if (!c) return nullptr;
    if (!c->active || c->gen != t.noteGen[idx & 7]) { t.note[idx & 7] = nullptr; return nullptr; }
    return c;
}

static void voice_release(Voice& v, u16 release) {
    if (!v.active) return;
    if (release) v.oscs[0].releaseDirect(release);
    if (v.oscs[0].isOsc()) v.oscs[0].release(); else v.stop_now();
    if (v.oscs[1].isOsc()) v.oscs[1].release();
}

static bool track_note_off(Track& t, u8 note, u16 release) {
    Voice* c = track_voice(t, note);
    if (c && c->bankProg == 0x00E9)   // drum-release tracer (bank0 prog233)
        NJLOG("noteOff DRUM t=%p ch=%u rel=%u age=%u st=%u\n",
              (void*)&t, note, release, c->age, c->oscs[0].state);
    if (!c) return false;
    voice_release(*c, release == 0 ? 0 : release);
    t.note[note & 7] = nullptr;
    return true;
}

// effective track params → push to voices (TTrack::updateTrack essentials)
static void track_update(Track& t) {
    float vol = t.timed[0].cur;
    if (t.volumeMode == 0) vol *= vol;
    if (t.mute) vol = 0.f;
    float pitch = pitch_to_ratio(t.timed[1].cur, (float)t.pitchRange);
    float pan = t.timed[3].cur, fx = t.timed[2].cur, dolby = t.timed[4].cur;
    for (int i = 0; i < 2; i++) {
        if (t.oscRoute[i] == 0xE) {
            float off = t.selfOsc[i].osc ? (t.selfOsc[i].phase * t.selfOsc[i].osc->width + t.selfOsc[i].osc->vertex) : 1.f;
            switch (t.selfOsc[i].osc ? t.selfOsc[i].osc->mode : 0) {
            case 1: pitch *= off; break;
            case 0: vol *= off; break;
            case 2: pan *= off; break;
            case 3: fx *= off; break;
            case 4: dolby *= off; break;
            }
        }
    }
    // TOuterParam application (decomp TTrack::updateTrack): outer multiplies/replaces the
    // track's own contribution before the parent combine. Pan uses panCalc with weight
    // panPower[3]/32767 (default 0x7FFF → outer pan replaces).
    if (t.outerSwitch & 1) vol *= t.outerVol;
    if (t.outerSwitch & 2) pitch *= t.outerPitch;
    if (t.outerSwitch & 8) {
        float w = t.panPower[3] / 32767.f;
        pan = pan * (1.f - w) + t.outerPan * w;
    }
    if (!t.parent || (t.trackMode & 1)) {
        t.chVol = vol; t.chPitch = pitch; t.chPan = pan; t.chFx = fx; t.chDolby = dolby;
    } else {
        t.chVol = t.parent->chVol * vol;
        t.chPitch = t.parent->chPitch * pitch;
        float w = t.panPower[4] / 32767.f;
        t.chPan = pan * (1.f - w) + t.parent->chPan * w;
        t.chFx = fx; t.chDolby = dolby;
    }
}
static void track_update_seq(Track& t, u32 flags, bool recursive) {
    u32 f = flags | t.updateFlags;
    t.updateFlags = 0;
    if (f) track_update(t);
    for (int i = 0; i < 16; i++)
        if (t.child[i] && t.child[i]->active) {
            if (recursive) track_update_seq(*t.child[i], f, true);
            else t.child[i]->updateFlags |= f;
        }
}

// gate/duration: seqTimeToDspTime
static s32 seq_to_dsp_time(Track& t, s32 time, u8 gatePct) {
    float res = (float)time * gatePct / 100.f;
    if (t.timeRelate) res /= (t.tickPerCall > 0 ? t.tickPerCall : 1.f);
    else res = res * 120.f / t.timebase * 7.f / 10.f;  // outputRate==0, subframes=7
    return (s32)res;
}

// BankMgr::noteOn port
static int track_note_on(Track& t, u8 chanIdx, s32 key, s32 vel, s32 gateTime) {
    if (t.mute && (t.pauseStatus & 0x40)) return -1;
    if (track_voice(t, chanIdx)) track_note_off(t, chanIdx, 0);

    const u16 reg6 = reg_read(t, 6);
    const u32 virtBank = (reg6 >> 8) & 0xFF;
    const u32 prog = reg6 & 0xFF;
    const int phys = g_data.vir2phy[virtBank];
    if (phys < 0 || phys >= (int)g_data.banks.size()) { NJLOG("noteOn: no bank virt=%u\n", virtBank); return -1; }
    Bank& bank = g_data.banks[phys];
    const Inst* inst = nullptr;
    float ivol = 1.f, ipitch = 1.f, randVolPitch = 1.f, volEffect = 1.f;
    u16 waveid = 0; u16 directRel = 0;
    bool fixed_pitch = false;
    float effPan = 0.5f, effFx = 0.f, effDolby = 0.f; (void)effFx; (void)effDolby;
    if (prog < 0x100) inst = bank.insts[prog];
    if (!inst) { NJLOG("noteOn: no inst bank=%u prog=%u\n", virtBank, prog); return -1; }

    const OscData* oscs[2] = {};
    int oscCount = 0;
    if (!inst->is_perc) {
        ivol = inst->vol; ipitch = inst->pitch;
        for (auto& o : inst->oscs) if (oscCount < 2) oscs[oscCount++] = &o;
        // effects (rand/sense)
        float pitchEff = 1.f;
        for (auto& ef : inst->effects) {
            float y;
            if (!ef.is_sense) {
                float r = (float)rand() / (float)RAND_MAX * 2.f - 0.9999999f;
                y = r * ef.rand_width + ef.rand_base;
            } else {
                int src = ef.sense_src == 1 ? vel : ef.sense_src == 2 ? key : 0;
                if (ef.sense_center == 0x7f || ef.sense_center == 0)
                    y = ef.sense_lo + src * (ef.sense_hi - ef.sense_lo) / 127.f;
                else if (src < ef.sense_center)
                    y = ef.sense_lo + (1.f - ef.sense_lo) * (src / (float)ef.sense_center);
                else
                    y = (ef.sense_hi - 1.f) * ((src - ef.sense_center) / (float)(0x7f - ef.sense_center)) + 1.f;
            }
            // Target mapping VERIFIED against the binary (BankMgr::noteOn 8030dec8..df7c:
            // vol *= instParam+0x3C = target-0 product; pitchRatio *= instParam+0x40 =
            // target-1 product). The decomp's JASBankMgr.cpp reads unk18 in BOTH spots —
            // transcription error; target 0 is VOLUME, target 1 is PITCH.
            if (ef.target == 0) volEffect *= y;
            else if (ef.target == 1) pitchEff *= y;
        }
        randVolPitch = pitchEff;
        const KeyRegion* km = nullptr;
        for (auto& k : inst->keys) if (key <= k.maxkey) { km = &k; break; }
        if (!km) return -1;
        const VeloRegion* vr = nullptr;
        for (auto& v : km->velo) if (vel <= v.maxvel) { vr = &v; break; }
        if (!vr) return -1;
        ivol *= vr->volmul;
        ipitch *= vr->pitchmul;
        waveid = vr->waveid;
    } else {
        if (key >= 0x80 || !inst->percs[key].valid) return -1;
        const auto& pc = inst->percs[key];
        ivol = pc.vol; ipitch = pc.pitch; directRel = pc.release;
        effPan = pc.pan;
        for (auto& ef : pc.effects) {              // perc rand effects (targets as inst)
            float r = (float)rand() / (float)RAND_MAX * 2.f - 0.9999999f;
            float y = r * ef.rand_width + ef.rand_base;
            if (ef.target == 0) volEffect *= y;
            else if (ef.target == 1) randVolPitch *= y;
        }
        const VeloRegion* vr = nullptr;
        for (auto& v : pc.velo) if (vel <= v.maxvel) { vr = &v; break; }
        if (!vr) return -1;
        ivol *= vr->volmul; ipitch *= vr->pitchmul;
        waveid = vr->waveid;
        fixed_pitch = true;   // percussion plays the wave at mapped pitch (no key scaling)
    }

    if (bank.wsys < 0 || bank.wsys >= (int)g_data.wsys_waves.size()) return -1;
    // group-aware wave lookup: the guest-selected resident scene group first (the
    // faithful choice — ids overlap between groups), then group 0 (stay waves), then
    // any group that has it (forgiving fallback for tee/start ordering races).
    WsysBank& wb = g_data.wsys_waves[bank.wsys];
    const Wave* wvp = nullptr;
    const int curScene = (bank.wsys < 8) ? g_wave_scene[bank.wsys].load(std::memory_order_relaxed) : 0;
    auto try_group = [&](int g) {
        if (wvp || g < 0 || g >= (int)wb.groups.size()) return;
        auto& t = wb.groups[g];
        if (waveid < t.size() && !t[waveid].pcm.empty()) wvp = &t[waveid];
    };
    try_group(curScene);
    try_group(0);
    for (int g = 0; !wvp && g < (int)wb.groups.size(); g++) try_group(g);
    if (!wvp) {
        NJLOG("noteOn: wave %u missing (wsys %d scene %d)\n", waveid, bank.wsys, curScene);
        return -1;
    }
    const Wave& wv = *wvp;

    // allocate voice
    Voice* v = nullptr;
    for (auto& cand : g_voices) if (!cand.active) { v = &cand; break; }
    if (!v) {
        // Voice stealing (the JAS DSP-channel allocator analogue: hardware reclaims
        // channels under load via priority stealing/breakLower). Dense sequences
        // (k_dolpic's mandolin tremolo) legitimately spawn notes faster than long
        // looping-wave releases decay — rejecting NEW notes would mute the melody to
        // keep old release tails. Steal the quietest RELEASING voice instead.
        float bestlvl = 1e9f;
        for (auto& cand : g_voices) {
            const u8 st = cand.oscs[0].state;
            if (st != 4 && st != 5 && st != 6) continue;   // release/forced/direct-release only
            const float lvl = cand.oscs[0].phase;
            if (lvl < bestlvl) { bestlvl = lvl; v = &cand; }
        }
        if (v) v->stop_now();
    }
    if (!v) {
        static u32 oov = 0;
        if (dbg() && (oov++ % 2000) == 0) {
            fprintf(stderr, "[njas] OUT OF VOICES #%u — table:\n", oov);
            for (int i = 0; i < kMaxVoices; i++) {
                Voice& d = g_voices[i];
                fprintf(stderr, "  v%02d own=%p st=%u osc0=%u gate=%d/%d loop=%u age=%u vol=%.2f\n",
                        i, (void*)d.owner, d.status, d.oscs[0].state, d.gate, d.gateLeft,
                        d.wave ? d.wave->loop : 0, d.age, d.vol);
            }
        }
        return -1;
    }
    *v = Voice();
    v->active = true;
    v->status = 1;
    v->gen = g_voice_gen++; if (!g_voice_gen) g_voice_gen = 1;
    v->owner = &t;
    v->wave = &wv;
    v->srcpos = 0;
    v->baseRatio = ipitch * (wv.rate / kDacRate) * randVolPitch;
    v->pitchRatio = v->baseRatio;
    if (!fixed_pitch) {
        int kk = key + 0x3C - wv.key;
        if (kk < 0) kk = 0; if (kk > 0x7f) kk = 0x7f;
        v->pitchRatio *= c5base(kk);
    }
    v->volBase = ivol;
    float vv = vel / 127.f;
    v->vol = v->volBase * vv * vv * volEffect;
    v->panSound = effPan;
    v->gate = gateTime == 0 ? -1 : gateTime;
    v->gateLeft = v->gate;
    // oscillators: bank oscs, then track overrides
    for (int i = 0; i < 2; i++) {
        v->oscs[i].init();
        if (i < oscCount && oscs[i]) { v->oscs[i].set(oscs[i]); v->oscs[i].initStart(); }
    }
    if (!v->oscs[0].isOsc()) {       // ensure an envelope exists (OSC_ENV equivalent)
        static OscData oscEnv;
        static bool oscEnvInit = false;
        if (!oscEnvInit) {
            static const s16 rel[6] = { 1, 0xA, 0, 0xf, 0, 0 };
            oscEnv.mode = 0; oscEnv.rate = 1.f; oscEnv.width = 1.f; oscEnv.vertex = 0.f;
            oscEnv.release.assign(rel, rel + 6);
            oscEnvInit = true;
        }
        v->oscs[0].set(&oscEnv);
        v->oscs[0].initStart();
    }
    if (directRel) v->oscs[0].releaseDirect(directRel);
    // Track oscillator overrides — TTrack::noteOn JASTrack.cpp:212 semantics. Only an
    // explicit oscroute (≠0xE/0xF) applies them; route ≥8 with a live inst osc KEEPS the
    // inst osc (copyOsc clones it back over the track data); route 4..7 keeps the inst
    // ATTACK and takes the track RELEASE; only an osc-less slot adopts the track osc
    // outright. Never initStart() here — the envelope is mid-flight and directRelease
    // (perc release) must survive (overwriteOsc never resets state on hardware). The
    // old behavior (any simpleadsr force-routed + initStart) restarted percussion under
    // the track's slow ADSR attack = the inaudible-drums bug (peak 0.0167).
    for (int i = 0; i < 2; i++) {
        u8 route = t.oscRoute[i];
        if (route == 0xF || route == 0xE) continue;
        u32 slot = route >= 8 ? route - 8 : route >= 4 ? route - 4 : route;
        if (slot >= 2) continue;
        const bool haveInst = v->oscs[slot].isOsc();
        if (route >= 8 && haveInst) continue;            // inst osc wins (copyOsc round-trip)
        v->trackOscCopy[i] = t.oscSrc[i];
        if (route >= 4 && route < 8 && haveInst) {       // inst attack + track release
            v->trackOscCopy[i].attack = v->oscs[slot].osc->attack;
            v->oscs[slot].set(&v->trackOscCopy[i]);
        } else if (haveInst) {
            v->oscs[slot].set(&v->trackOscCopy[i]);      // swap data, keep state
        } else {
            v->oscs[slot].set(&v->trackOscCopy[i]);
            v->oscs[slot].initStart();                   // never started — start now
            if (directRel && slot == 0) v->oscs[0].releaseDirect(directRel);
        }
    }
    v->key = (u8)key; v->vel = (u8)vel; v->bankProg = reg6;
    if (FILE* ab = ab_out()) {                 // A/B: voice-on with full cause context
        u32 soundId = 0;
        for (Track* p = &t; p && !soundId; p = p->parent)
            for (auto& a : g_active)
                if (a.used && a.worker == p) { soundId = a.id; break; }
        v->abOpen = true; v->abPeak = 0.f;
        v->abSeqOff = (u32)(t.seq.cur - t.seq.base);
        v->abSoundId = soundId;
        fprintf(ab, "{\"ev\":\"von\",\"t\":%.1f,\"hash\":\"%08x\",\"ratio\":%u,"
                    "\"bank\":%u,\"prog\":%u,\"key\":%d,\"vel\":%d,\"seqoff\":\"%x\","
                    "\"id\":\"%08x\"}\n",
                ab_now_ms(), wv.srcHash, (unsigned)(v->pitchRatio * 4096.f + 0.5f),
                virtBank, prog, key, vel, v->abSeqOff, soundId);
        fflush(ab);
    }
    t.note[chanIdx & 7] = v;
    t.noteGen[chanIdx & 7] = v->gen;
    track_update(t);
    NJLOG("noteOn t=%p ch=%u key=%d vel=%d bank=%u prog=%u wave=%u rate=%.0f ratio=%.3f gate=%d chvol=%.4f notevol=%.4f ivol=%.4f\n",
          (void*)&t, chanIdx, key, vel, virtBank, prog, waveid, wv.rate, v->pitchRatio, gateTime,
          t.chVol, v->vol, ivol);
    return 0;
}

// ====================== BMS interpreter (TSeqParser port) ======================

// Arglist (JASSeqParser) — index = opcode - 0xC0
struct ArgSpec { u8 argc; u16 mask; };
static const ArgSpec kArgs[64] = {
    {0,0},{2,8},{2,8},{1,2},{0,0},{0,0},{1,0},{1,2},{0,0},{1,1},{0,0},{2,0},
    {2,0xC},{1,0},{1,0},{1,3},{2,5},{2,0xC},{2,0xC},{0,0},{1,0},{1,0},{1,0},{2,8},
    {5,0x155},{1,0},{1,0},{1,0},{1,1},{2,4},{1,0},{2,8},{1,0},{0,0},{0,0},{0,0},
    {2,4},{0,0},{0,0},{1,1},{0,0},{0,0},{1,2},{5,0},{4,0x55},{1,2},{1,2},{3,0},
    {1,0},{1,0},{3,0x28},{1,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{1,1},{0,0},
    {0,0},{1,1},{1,1},{0,0},
};

static bool cond_check(Track& t, u8 cond) {
    u16 v = reg_read(t, 3);
    switch (cond & 0xF) {
    case 0: return true;
    case 1: return v == 0;
    case 2: return v != 0;
    case 3: return v == 1;
    case 4: return v >= 0x8000;
    case 5: return v < 0x8000;
    }
    return false;
}

static int track_open_child(Track& t, u8 idx, u8 mode, u32 off);

static void write_time_param(Track& t, u8 param) {
    u8 target = t.seq.readByte();
    s16 value = 0;
    switch (param & 0xC) {
    case 0: value = (s16)reg_read(t, t.seq.readByte()); break;
    case 4: value = t.seq.readByte(); break;
    case 8: { u16 b = t.seq.readByte(); value = (b & 0x80) ? (s16)(b << 8) : (s16)((b << 8) | (b << 1)); break; }
    case 0xC: value = (s16)t.seq.read16(); break;
    }
    s32 time = 0;
    switch (param & 3) {
    case 0: time = -1; break;
    case 1: time = reg_read(t, t.seq.readByte()); break;
    case 2: time = t.seq.readByte(); break;
    case 3: time = t.seq.read16(); break;
    }
    if (target >= 18) return;
    MoveParam& mp = t.timed[target];
    mp.target = value / 32768.f;
    if (time <= 0) { mp.cur = mp.target; mp.amount = 0.f; mp.time = 1.f; }
    else { mp.amount = (mp.target - mp.cur) / time; mp.time = (float)time; }
}

static void write_reg_param(Track& t, u8 param) {
    u8 mode = param & 0xC;
    u8 op = param & 0x3;
    u16 tblElem = 0;
    u32 tblBase = 0;
    if ((param & 0xF) == 0xB) { mode = 0; op = 0xB; }
    if ((param & 0xF) == 0xA) {
        op = 10;
        param = t.seq.readByte();
        mode = param & 0xC;
        tblElem = (param >> 4) + 4;
    }
    if ((param & 0xF) == 0x9) {
        op = t.seq.readByte();
        mode = op & 0xC;
        op &= 0xF0;
        if (mode == 8) mode = 0x10;
    }
    u8 target = t.seq.readByte();
    if (op == 10) tblBase = reg_read32(t, t.seq.readByte());
    s32 val = 0;
    switch (mode) {
    case 0: val = reg_read(t, t.seq.readByte()); break;
    case 4: val = t.seq.readByte(); break;
    case 8: { u16 b = t.seq.readByte(); val = (b & 0x80) ? (b << 8) : ((b << 8) | (b << 1)); break; }
    case 0xC: val = t.seq.read16(); break;
    case 0x10: val = 0xffff; break;
    }
    s32 curv = reg_read(t, target);
    switch (op) {
    case 0x1: if (mode == 4) val = (s16)extend8to16((u8)val); val = curv + val; break;
    case 0x2: reg_write(t, 4, (u16)((curv * val) >> 16)); reg_write(t, 5, (u16)(curv * val)); break;
    case 0x3: t.reg[3] = (u16)(curv - val); return;
    case 0xA: {
        u32 r;
        switch (tblElem) {
        case 4: r = t.seq.getByte(tblBase + val); break;
        case 5: r = t.seq.get16(tblBase + 2 * val); break;
        case 6: r = t.seq.get24(tblBase + 3 * val); break;
        case 7: r = t.seq.get32(tblBase + 4 * val); break;
        default: r = t.seq.get32(tblBase + val); break;
        }
        val = (s32)r;
        break;
    }
    case 0x10: case 0x20:
        if (mode == 4) val = (s16)extend8to16((u8)val);
        val = val < 0 ? (curv >> -val) : (curv << val);
        break;
    case 0x30: val = curv & val; break;
    case 0x40: val = curv | val; break;
    case 0x50: val = curv ^ val; break;
    case 0x60: val = -curv; break;
    case 0x90: { s32 r = rand(); val = r - (r / val) * val; break; }
    }
    u32 tgt = target;
    u16 mirror = (u16)val;
    switch (tgt) {
    case 0x21: val = ((t.bankProg >> 8) << 8) | (val & 0xff); tgt = 6; break;
    case 0x20: val = (t.bankProg & 0xff) | (val << 8); tgt = 6; break;
    case 0x22: reg_write(t, 0, (u16)(val >> 8)); val &= 0xff; tgt = 1; mirror = (u16)val; break;
    default: break;
    }
    if (tgt == 6) {
        t.bankProg = (u16)val;
        // bank/prog write resets osc routes (TTrack setParam: route → 0xF unless 0xE)
        for (int i = 0; i < 2; i++) if (t.oscRoute[i] != 0xE) t.oscRoute[i] = 0xF;
    }
    if (tgt < 48) t.reg[tgt] = (u16)val;
    t.reg[3] = mirror;
    if (tgt == 7) t.updateFlags |= 2;
}

// returns parser retcode: 0 continue, 1 yield (wait), 2 reti, 3 close-track
static int run_cmd(Track& t, u8 op, u16 maskExtra);

static int cmd_note_on(Track& t, u8 note) {
    u8 key = note + t.transposeTotal;
    u8 flag = t.seq.readByte();
    if (flag & 0x80) { key = (u8)reg_exchange(t, note); key += t.transposeTotal; }
    u8 sweepFrom = 0;
    if ((flag >> 5) & 0x2) { sweepFrom = key; key = t.lastNote; }
    u8 vel = t.seq.readByte();
    if (vel >= 0x80) vel = (u8)reg_exchange(t, vel - 0x80);
    u32 dur; u8 gatePct; u8 chan;
    if (!(flag & 0x7)) {
        chan = 0;
        gatePct = t.seq.readByte();
        if (gatePct >= 0x80) gatePct = (u8)reg_exchange(t, gatePct - 0x80);
        dur = 0;
        for (u8 i = 0; i < ((flag >> 3) & 0x3); i++) { dur = (dur << 8) | t.seq.readByte(); }
        if (((flag >> 3) & 0x3) == 1 && dur >= 0x80) dur = reg_exchange(t, (u8)(dur - 0x80));
    } else {
        chan = flag & 0x7;
        if ((flag >> 3) & 0x3) chan = (u8)reg_exchange(t, chan - 1);
        dur = 0xFFFFFFFF;
        gatePct = 100;
    }
    t.connectCase = (flag >> 5) & 0x3;
    s32 dspTime = (s32)dur;
    if (t.beforeTie) {
        if (t.connectCase & 1) dspTime = -1;
        if (dspTime != -1) dspTime = seq_to_dsp_time(t, dspTime, gatePct);
        // gateOn: retarget existing channel — M1: treat as fresh note if channel gone
        Voice* c = track_voice(t, chan);
        if (!c) NJLOG("tie LOST t=%p ch=%u key=%d (no live channel)\n", (void*)&t, chan, key);
        else if (c->bankProg == 0x00E9)
            NJLOG("tie DRUM t=%p ch=%u key=%d age=%u st=%u ph=%.4f\n",
                  (void*)&t, chan, key, c->age, c->oscs[0].state, c->oscs[0].phase);
        if (c) {
            int kk = key + 0x3C - (c->wave ? c->wave->key : 60);
            if (kk < 0) kk = 0; if (kk > 0x7f) kk = 0x7f;
            c->pitchRatio = c->baseRatio * c5base(kk);
            float vvv = vel / 127.f;
            c->vol = c->volBase * vvv * vvv;
            if (dspTime > 0) { c->gate = dspTime; c->gateLeft = dspTime; }
        }
    } else {
        if (dspTime != -1) dspTime = seq_to_dsp_time(t, dspTime, gatePct);
        if (t.connectCase & 1) dspTime = -1;
        if (!t.paused || !(t.pauseStatus & 0x10))
            track_note_on(t, chan, key, vel, dspTime);
    }
    t.baseTime = dur;
    t.beforeTie = (t.connectCase & 1) != 0;
    if (t.connectCase & 0x2) {
        // key sweep target — M1 simplification: jump pitch at sweep end not modeled
        key = sweepFrom;
    }
    t.lastNote = key;
    if (dur == 0xFFFFFFFF) return 0;
    t.seq.wait(dur ? (s32)dur : -1);
    return 1;
}

static int cmd_note_off(Track& t, u8 flag) {
    if (flag == 0xF9) {
        u32 r = t.seq.readByte();
        u8 rd = (u8)reg_exchange(t, r & 0x7);
        if (rd > 7 || rd == 0) { if (r & 0x80) t.seq.cur++; return 0; }
        flag = rd + 0x80;
        if (r & 0x80) flag |= 0x08;
    }
    u8 note = flag & 0xF;
    s32 release = 0;
    if (flag & 0x8) {
        note -= 0x8;
        release = t.seq.readByte();
        if (release > 100) release = (release - 98) * 20;
    }
    track_note_off(t, note, (u16)release);
    return 0;
}

static int cmd_wait_imm(Track& t, u8 flag) {
    int n = flag == 0x80 ? 1 : 2;
    s32 v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | t.seq.readByte();
    t.seq.waitTimer = v;
    return v ? 1 : 0;
}

static int parser_main(Track& t) {
    for (;;) {
        u8 op = t.seq.readByte();
        int rc = 0;
        if (!(op & 0x80)) rc = cmd_note_on(t, op);
        else if ((op & 0xF0) == 0x80 && !(op & 0x07)) rc = cmd_wait_imm(t, op);
        else if ((op & 0xF0) == 0x80 || op == 0xF9) rc = cmd_note_off(t, op);
        else switch (op & 0xF0) {
        case 0x90: write_time_param(t, op & 0xF); break;
        case 0xA0: write_reg_param(t, op & 0xF); break;
        case 0xB0: {
            u8 r5 = t.seq.readByte();
            const int regForm = (op & 8) ? 1 : 0;
            if (regForm) r5 = (u8)reg_exchange(t, r5);
            u16 mask = 0;
            if (!regForm || (op & 7)) {
                u8 b = t.seq.readByte();
                u16 bit = 3;
                for (u32 i = 0; i < (u32)(op & 7) + 1; i++) {
                    if (b & 0x80) mask |= bit;
                    b <<= 1; bit <<= 2;
                }
            }
            rc = run_cmd(t, r5, mask);
            break;
        }
        default: rc = run_cmd(t, op, 0); break;
        }
        if (rc == 0) continue;
        if (rc == 1) break;
        if (rc == 2) return -2;
        if (rc == 3) return -1;
    }
    return 0;
}

static int run_cmd(Track& t, u8 op, u16 maskExtra) {
    if (op < 0xC0) return 0;
    const u8 idx = op - 0xC0;

    // call / jmp: custom argument reading (cmdCall / cmdJmp)
    if (op == 0xC4 || op == 0xC8) {
        u8 flag = t.seq.readByte();
        u32 offset = 0;
        bool have = true;
        if (flag & 0x80) {
            u8 r = t.seq.readByte();
            if (flag & 0x40) {
                u32 base;
                if (flag & 0x20) base = reg_read(t, t.seq.readByte());
                else base = t.seq.read24();
                offset = t.seq.get24(base + reg_read32(t, r) * 3);
            } else {
                offset = reg_read32(t, r);
            }
        } else offset = t.seq.read24();
        (void)have;
        if (cond_check(t, flag)) {
            if (op == 0xC4) t.seq.call(offset);
            else t.seq.jump(offset);
        }
        return 0;
    }
    if (op == 0xFB) {  // printf: consume
        u32 cnt = 0;
        for (;;) {
            u8 c = t.seq.readByte();
            if (!c) break;
            if (c == '\\') { if (!t.seq.readByte()) break; continue; }
            if (c == '%') { u8 c2 = t.seq.readByte(); if (!c2) break; if (strchr("dxsrRt", c2)) cnt++; }
        }
        for (u32 i = 0; i < cnt; i++) t.seq.readByte();
        return 0;
    }

    const ArgSpec spec = kArgs[idx];
    u16 mask = spec.mask | maskExtra;
    u32 a[8] = {};
    for (u8 i = 0; i < spec.argc; i++) {
        switch (mask & 3) {
        case 0: a[i] = t.seq.readByte(); break;
        case 1: a[i] = t.seq.read16(); break;
        case 2: a[i] = t.seq.read24(); break;
        case 3: a[i] = reg_exchange(t, t.seq.readByte()); break;
        }
        mask >>= 2;
    }

    switch (op) {
    case 0xC1: { // opentrack
        u8 b1 = a[0] & 0xF;
        u8 b2 = (a[0] >> 6) & 3;
        if (a[0] & 0x20) b2 = 4;
        track_open_child(t, b1, b2, a[1]);
        return 0;
    }
    case 0xC2: { // opentrackbros
        if (!t.parent) return 0;
        u8 b1 = a[0] & 0xF;
        u8 b2 = (a[0] >> 6) & 3;
        if (a[0] & 0x20) b2 = 4;
        track_open_child(*t.parent, b1, b2, a[1]);
        return 0;
    }
    case 0xC6:   // ret
        if (cond_check(t, (u8)a[0])) {
            if (t.seq.loopIdx == 0 || !t.seq.ret()) return 3;
        }
        return 0;
    case 0xC9: t.seq.loopS((u16)a[0]); return 0;
    case 0xCA: t.seq.loopE(); return 0;
    case 0xCB: reg_write(t, (u8)a[1], t.portValue[a[0] & 15]); t.portImport[a[0] & 15] = 0; return 0;
    case 0xCC: t.portExport[a[0] & 15] = 1; t.portValue[a[0] & 15] = (u16)a[1]; return 0;
    case 0xCD: reg_write(t, 3, t.portImport[a[0] & 15]); return 0;
    case 0xCE: reg_write(t, 3, t.portExport[a[0] & 15]); return 0;
    case 0xCF: t.seq.wait((s32)a[0]); return a[0] ? 1 : 0;
    case 0xD0: return 0;                       // connectname
    case 0xD1: if (t.parent) t.parent->writePortImport(a[0] & 0xf, (u16)a[1]); return 0;
    case 0xD2: { Track* c = t.child[(a[0] >> 4) & 0xF]; if (c) c->writePortImport(a[0] & 0xf, (u16)a[1]); return 0; }
    case 0xD4: t.lastNote = (u8)(a[0] + t.transposeTotal); return 0;
    case 0xD5: t.timeRelate = a[0] ? 1 : 0; return 0;
    case 0xD6:   // simpleosc
        if (a[0] == 0) t.oscSrc[1] = kVibratoDef;
        return 0;
    case 0xD7:   // simpleenv
        if (a[0] == 0) {
            t.oscSrc[0] = kEnvelopeDef;
            t.oscSrc[0].attack.clear();
            const u8* p = t.seq.base + a[1];
            for (;;) { s16 m = (s16)be16(p), ti = (s16)be16(p + 2), va = (s16)be16(p + 4);
                t.oscSrc[0].attack.push_back(m); t.oscSrc[0].attack.push_back(ti); t.oscSrc[0].attack.push_back(va);
                p += 6; if (m > 0xa) break; }
            // NOTE: does NOT set oscRoute — only cmdOscRoute does (JASSeqParser).
        }
        return 0;
    case 0xD8: { // simpleadsr (A dt, A target, S dt, S target?, release)
        s16 ar[5]; for (int i = 0; i < 5; i++) ar[i] = (s16)a[i];
        t.oscSrc[0].mode = 0; t.oscSrc[0].rate = 1.f; t.oscSrc[0].width = 1.f; t.oscSrc[0].vertex = 0.f;
        s16 ads[12]; memcpy(ads, t.adsTable, sizeof(ads));
        memcpy(ads, kAdsDef, sizeof(ads));
        ads[1] = ar[0]; ads[4] = ar[1]; ads[7] = ar[2]; ads[8] = ar[3];
        s16 rel[6]; memcpy(rel, kRelDef, sizeof(rel));
        rel[1] = ar[4];
        memcpy(t.adsTable, ads, sizeof(ads));
        memcpy(t.relTable, rel, sizeof(rel));
        t.oscSrc[0].attack.assign(t.adsTable, t.adsTable + 12);
        t.oscSrc[0].release.assign(t.relTable, t.relTable + 6);
        // NOTE: cmdSimpleADSR does NOT route the osc (JASSeqParser.cpp:309) — the BMS
        // must issue oscroute for it to affect notes. Forcing route 8 here hijacked
        // percussion envelopes (the inaudible-BGM-drums bug).
        return 0;
    }
    case 0xD9:   // transpose
        t.transposeSelf = (s8)a[0];
        t.transposeTotal = t.transposeSelf + (t.parent ? t.parent->transposeSelf : 0);
        return 0;
    case 0xDA: { // closetrack
        Track* c = t.child[a[0] & 0xF];
        if (c) {
            // close: stop notes, recurse
            for (int i = 0; i < 8; i++) track_note_off(*c, (u8)i, 0);
            c->active = 0; c->used = false;
            for (int i = 0; i < 16; i++)
                if (c->child[i]) { c->child[i]->active = 0; c->child[i]->used = false; c->child[i] = nullptr; }
            t.child[a[0] & 0xF] = nullptr;
        }
        return 0;
    }
    case 0xDB: return 0;                       // outswitch
    case 0xDC: track_update(t); return 0;      // updatesync
    case 0xDD: return 0;                       // busconnect
    case 0xDE: t.pauseStatus = (u8)a[0]; return 0;
    case 0xDF: t.intr.setIntr(a[0] & 7, a[1]); return 0;
    case 0xE0: t.intr.resetIntr(a[0] & 7); return 0;
    case 0xE1: t.intr.enabled = 1; t.seq.intrRet = nullptr; return 0;       // clri
    case 0xE2: t.intr.enabled = 0; return 0;   // seti
    case 0xE3: t.intr.enabled = 1; t.seq.retIntr(); return 2;               // reti
    case 0xE4: t.intr.timer = (u16)a[0]; t.intr.timerPeriod = (u16)a[1]; t.intr.timerCount = (u16)a[0]; return 0;
    case 0xE6: return 0;                       // connectopen
    case 0xE7: return 0;                       // connectclose
    case 0xE9: reg_write(t, 3, 0); return 0;   // synccpu (no JAI callback natively)
    case 0xEA:   // flushall
        for (int i = 0; i < 8; i++) track_note_off(t, (u8)i, 0);
        return 0;
    case 0xEB: return 0;                       // flushrelease
    case 0xEC: t.seq.wait((s32)a[0]); return a[0] ? 1 : 0;   // wait3 (reg/long forms)
    case 0xED:   // panpowset
        for (int i = 0; i < 3; i++) t.panPower[i] = (s16)a[i];
        t.panPower[3] = (s16)(a[3] * 327.67f);
        t.panPower[4] = (s16)(a[4] * 327.67f);
        return 0;
    case 0xEE: return 0;                       // iirset
    case 0xEF: return 0;                       // firset
    case 0xF0: return 0;                       // extset
    case 0xF1: return 0;                       // panswset
    case 0xF2: { // oscroute
        u32 i = (a[0] >> 4) & 0xF;
        u32 v = a[0] & 0xF;
        if (i < 2) {
            t.oscRoute[i] = (u8)v;
            if (v == 0xE) { t.selfOsc[i].set(&t.oscSrc[i]); t.selfOsc[i].initStart(); }
        }
        return 0;
    }
    case 0xF3: return 0;                       // iircutoff
    case 0xF4: { // oscfull
        u32 var1 = (a[0] & 0x10) >> 4;
        int var2 = a[0] & 0x0f;
        if (var1 < 2) {
            OscData& o = t.oscSrc[var1];
            if (a[0] & 0x80) {
                o = OscData();
                o.mode = (u8)var2; o.rate = 1.f; o.width = 1.f; o.vertex = 0.f;
                if (var2 == 1) o.vertex = 1.f;
            }
            if (a[0] & 0x40) {
                o.attack.clear();
                if (a[1]) {
                    const u8* p = t.seq.base + a[1];
                    for (;;) { s16 m = (s16)be16(p), ti = (s16)be16(p + 2), va = (s16)be16(p + 4);
                        o.attack.push_back(m); o.attack.push_back(ti); o.attack.push_back(va);
                        p += 6; if (m > 0xa) break; }
                }
            }
            if (a[0] & 0x20) {
                o.release.clear();
                if (a[2]) {
                    const u8* p = t.seq.base + a[2];
                    for (;;) { s16 m = (s16)be16(p), ti = (s16)be16(p + 2), va = (s16)be16(p + 4);
                        o.release.push_back(m); o.release.push_back(ti); o.release.push_back(va);
                        p += 6; if (m > 0xa) break; }
                }
            }
        }
        return 0;
    }
    case 0xF5: t.volumeMode = (u8)a[0]; return 0;
    case 0xFA: reg_write(t, 3, 0); return 0;   // checkwave
    case 0xFC: return 0;                       // nop
    case 0xFD: t.tempo = (u16)a[0]; if (!t.parent) track_update_tempo(t); else t.tempoChild = 1; return 0;
    case 0xFE: t.timebase = (u16)a[0]; if (!t.parent) track_update_tempo(t); return 0;
    case 0xFF: { // finish: advance timed params, then yield-close
        for (int i = 0; i < 18; i++) {
            MoveParam& mp = t.timed[i];
            if (mp.time > 0.f) { mp.cur += mp.amount; mp.time -= 1.f; }
        }
        track_update_seq(t, 0xFFFF, true);
        return 3;
    }
    default:
        NJLOG("unhandled cmd %02x\n", op);
        return 0;
    }
}

static int track_open_child(Track& t, u8 idx, u8 mode, u32 off) {
    idx &= 0xF;
    if (t.child[idx]) {
        Track* c = t.child[idx];
        for (int i = 0; i < 8; i++) track_note_off(*c, (u8)i, 0);
        c->active = 0; c->used = false;
        t.child[idx] = nullptr;
    }
    Track* c = track_alloc();
    if (!c) return -1;
    c->trackMode = mode;
    track_init(*c, t.seq.base, off, &t);
    track_update(*c);
    t.child[idx] = c;
    return 0;
}

// TTrack::mainProc port (per-tick)
static s8 track_main(Track& t) {
    if (t.parent && t.tempoChild == 1) {
        float r = (float)t.tempo / t.parent->tempo;
        if (r > 1.f) r = 1.f;
        t.tickAcc += r;
        if (t.tickAcc < 1.f) return 0;
        t.tickAcc -= 1.f;
    }
    t.transposeTotal = t.transposeSelf + (t.parent ? t.parent->transposeSelf : 0);
    t.intr.request(7);
    t.intr.timerProcess();
    s32 rc;
    do {
        if (!t.seq.intrRet) {
            s32 h = t.intr.check();
            if (h >= 0) t.seq.callIntr(t.seq.base + h);
        }
        if (t.paused && (t.pauseStatus & 2)) goto bail;
        if (t.seq.waitTimer == -1) {
            Voice* c = track_voice(t, 0);
            bool done = !c || c->status == 0xFF;
            if (!done) break;
            t.seq.waitTimer = 0;
        }
        if (t.seq.waitTimer > 0) {
            if (!t.seq.waitCountDown()) break;
            // endProcess
            if (t.baseTime != 0xffffffff && t.connectCase == 0) t.note[0] = nullptr;
        }
        rc = parser_main(t);
        if (rc == -1) return -1;
    } while (rc == -2);

    {
        u32 r31 = 0;
        for (int i = 0; i < 18; i++) {
            MoveParam& mp = t.timed[i];
            if (mp.time > 0.f) { mp.cur += mp.amount; mp.time -= 1.f; r31 |= 1u << i; }
        }
        t.updateFlags |= r31;
    }
bail:
    track_update_seq(t, 0, false);
    for (int i = 0; i < 16; i++) {
        Track* c = t.child[i];
        if (c && c->active) {
            if (track_main(*c) == -1) {
                for (int j = 0; j < 8; j++) track_note_off(*c, (u8)j, 0);
                c->active = 0; c->used = false;
                t.child[i] = nullptr;
            }
        }
    }
    return 0;
}

static void track_inc_self_osc(Track& t) {
    for (int i = 0; i < 2; i++)
        if (t.oscRoute[i] == 0xE && t.selfOsc[i].isOsc()) t.selfOsc[i].getOffset();
    for (int i = 0; i < 16; i++)
        if (t.child[i]) track_inc_self_osc(*t.child[i]);
}

// ---- audio A/B harness event stream (docs/audio_ab_harness.md) ----------------
// SUNBRIGHT_AB_EVENTS=<path>: JSONL voice + anchor events. The native side carries
// the CAUSE context (bank:prog/key/vel, BMS offset, owning sound id) that turns a
// "voice differs" into a fixable report.
static FILE* ab_out() {
    static FILE* f = [] {
        const char* p = getenv("SUNBRIGHT_AB_EVENTS");
        return p ? fopen(p, "w") : nullptr;
    }();
    return f;
}
static double ab_now_ms() {
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration_cast<duration<double, std::milli>>(steady_clock::now() - t0).count();
}
static void ab_anchor(const char* kind, u32 id, const char* name) {
    if (FILE* f = ab_out()) {
        fprintf(f, "{\"ev\":\"%s\",\"t\":%.1f,\"id\":\"%08x\"%s%s%s}\n", kind, ab_now_ms(),
                id, name ? ",\"name\":\"" : "", name ? name : "", name ? "\"" : "");
        fflush(f);
    }
}

// ============================ engine driver ============================

static std::mutex g_mtx;
static bool g_inited = false, g_init_failed = false;
static float g_mixbuf[kSub * 2];
static int g_mix_have = 0, g_mix_pos = 0;
static FILE* g_solo_dump = nullptr;   // SUNBRIGHT_DUMP_NJAS: engine-only wav

struct PendingSE { u32 id; u32 swbit; u8 prio; bool isBgm = false; u16 age = 0; u32 posPtr = 0; };
static std::vector<PendingSE> g_pending;

// ---- M2: SE handle layer (category volumes, per-sound move params, stop/fade).
// Mirrors the JAI side that cannot reach JAS under recomp: JAISound handle ops arrive
// via the se_native.cpp tees keyed by sound id; per JAI frame (60 Hz) the move-param
// products are flushed to the worker track's TOuterParam, exactly like
// JAIBasic::sendSeAllParameter → JAISystemInterface::setSePortParameter.
static float g_catvol[256];
static bool g_catvol_init = false;

struct MovePara {                        // JAISound::initMoveParameter semantics
    float cur = 1.f, target = 1.f, step = 0.f; u32 left = 0;
    void set(float v, u32 time) {
        target = v;
        if (time == 0) { cur = v; left = 0; step = 0.f; return; }
        step = (v - cur) / (float)time; left = time;
    }
    void tick() { if (left) { cur += step; if (--left == 0) cur = target; } }
};
struct ActiveSE {
    bool used = false;
    bool isBgm = false;                  // worker = a BGM root track, not an SE worker
    Track* worker = nullptr;
    u32 id = 0, swbit = 0;
    u8 prio = 0;
    bool seenBusy = false;               // worker's port2 went nonzero since dispatch
    u16 startAge = 0;                    // ticks since dispatch (busy-detect grace)
    MovePara vol[9], pitch[9], pan[9];   // pitch/pan slots init neutral below
    bool panTouched = false;
    s32 stopFade = -1;                   // >=0: fading to stop, counts down
    u32 posPtr = 0;                      // game-world position Vec* (JAIActor.pos)
    u16 sinceReq = 0;                    // ticks since last (re-)request (JAISound lifeTime)
};
static ActiveSE g_active[32];

// all root tracks ticked by render_subframe: g_root (init/SE BMS) + one per playing BGM
static std::vector<Track*> g_players;

// recursive close: silence notes, free the whole child tree (TTrack::close semantics)
static void track_close_recursive(Track& t) {
    for (int i = 0; i < 8; i++) track_note_off(t, (u8)i, 0);
    for (int i = 0; i < 16; i++)
        if (t.child[i]) { track_close_recursive(*t.child[i]); t.child[i] = nullptr; }
    t.active = 0; t.used = false;
}

static void active_init(ActiveSE& a, Track* w, u32 id, u32 swbit, u8 prio) {
    a = ActiveSE();
    a.used = true; a.worker = w; a.id = id; a.swbit = swbit; a.prio = prio;
    for (int i = 0; i < 9; i++) { a.pan[i].cur = a.pan[i].target = 0.5f; }
}
static void worker_outer_reset(Track* w) {
    w->outerSwitch = 0; w->outerVol = 1.f; w->outerPitch = 1.f; w->outerPan = 0.5f;
}
static void active_stop_now(ActiveSE& a) {
    if (a.worker) {
        if (a.isBgm) {
            // stopSeq: tear down the whole BGM root tree and unregister the player
            track_close_recursive(*a.worker);
            for (auto it = g_players.begin(); it != g_players.end();)
                it = (*it == a.worker) ? g_players.erase(it) : it + 1;
        } else {
            a.worker->writePortImport(0, 0);             // looping snippets poll port0
            for (int i = 0; i < 8; i++) track_note_off(*a.worker, (u8)i, 0);
            worker_outer_reset(a.worker);
        }
    }
    a.used = false;
}
static ActiveSE* active_find(u32 id) {
    for (auto& a : g_active) if (a.used && a.id == id) return &a;
    return nullptr;
}

// ---- M2.6 native 3D SE layer (PC-native port of MSHandle's distance code) ----------
// The engine owns cameras, sound positions, and the SMS curves. Inputs are GAME data
// captured at API seams (setCameraInfo tee = camera pos/view-mtx pointers; JAIActor
// position Vec* from the startSoundBasic tee) — guest AUDIO state is never read.
// Sources: reference/sms/src/MSound/MSHandle.cpp (smSeCategory, MSACos, calcVolume,
// calcPan, setSeDistance*) — SMS OVERRIDES the generic JAISound curves; per-frame
// transform per JAIGFrameSe.cpp: camSpace = ViewMtx × worldPos, dist = |camSpace|.
// Not modeled yet: dolby/fir/fxmix buses (engine has no such buses), doppler (slot 1),
// the swbit-0xC0 pitch-offset term (JAISound.unk3, no native source yet).
struct SeCategory { u8 type; float dist, vol, falloff7; };
static const SeCategory kSeCategory[16] = {            // MSHandle::smSeCategory
    { 2,  8000.f, 0.76f, 150.f }, { 2,  8000.f, 1.f,   150.f },
    { 2,  6000.f, 1.f,   500.f }, { 3,  6000.f, 0.81f, 500.f },
    { 2, 12000.f, 0.84f, 500.f }, { 4, 12000.f, 0.59f, 500.f },
    { 2,  7000.f, 0.90f, 500.f }, { 2,  8000.f, 1.f,   500.f },
    { 2,  6000.f, 0.76f, 500.f }, { 2,  8000.f, 1.f,   500.f },
    { 2,  8000.f, 1.f,   500.f }, { 2,  8000.f, 1.f,   500.f },
    { 2,  8000.f, 1.f,   500.f }, { 2,  8000.f, 1.f,   500.f },
    { 2,  8000.f, 1.f,   500.f }, { 2,  8000.f, 1.f,   500.f },
};
static const float kACosPrm[101] = {                   // MSHandle::smACosPrm
    3.141592f,   2.941258f,   2.857799f,   2.793427f,   2.738877f,   2.690566f,
    2.6466579f,  2.606066f,   2.568079f,   2.532207f,   2.4980919f,  2.465462f,
    2.434109f,   2.403867f,   2.374599f,   2.346194f,   2.3185589f,  2.291615f,
    2.265295f,   2.2395389f,  2.214298f,   2.1895249f,  2.165182f,   2.141233f,
    2.1176469f,  2.0943949f,  2.0714509f,  2.0487909f,  2.026395f,   2.004241f,
    1.982313f,   1.960593f,   1.939064f,   1.917713f,   1.896526f,   1.875489f,
    1.854591f,   1.833819f,   1.813162f,   1.792611f,   1.772154f,   1.751783f,
    1.731487f,   1.711258f,   1.691086f,   1.670964f,   1.650882f,   1.630832f,
    1.6108069f,  1.590798f,   1.570796f,   1.550795f,   1.530786f,   1.5107599f,
    1.490711f,   1.470629f,   1.450507f,   1.430335f,   1.4101059f,  1.38981f,
    1.369439f,   1.348982f,   1.328431f,   1.3077739f,  1.287002f,   1.266104f,
    1.245067f,   1.223879f,   1.202528f,   1.181f,      1.1592799f,  1.137351f,
    1.115198f,   1.092801f,   1.070142f,   1.047198f,   1.023945f,   1.000359f,
    0.97641098f, 0.95206797f, 0.927295f,   0.902054f,   0.876298f,   0.849978f,
    0.82303399f, 0.795399f,   0.766994f,   0.73772597f, 0.70748299f, 0.676131f,
    0.64350098f, 0.609386f,   0.57351297f, 0.53552699f, 0.49493399f, 0.451027f,
    0.402716f,   0.34816599f, 0.28379399f, 0.200335f,   0.0f,
};
static constexpr float kPanMaxAmp = 0.499f, kPanCAdjust = 0.02f, kPanCShift = 1.6394f,
                       kPanHiSenceDist = 12.f, kMSDistMaxSence = 0.5f,
                       kMaxVolumeDistance = 1200.f;     // MSound init: setParamMaxVolumeDistance
struct AudioCam { std::atomic<u32> pos{0}, mtx{0}; };   // guest Vec* / Mtx* (game-owned)
static AudioCam g_cam;                                  // SMS: audioCameraMax == 1

static u32 se_cat_of(u32 id) {                          // MSHandle get_thing
    const u32 top = id >> 30;
    if (top == 0) return (id >> 12) & 0xF;
    return 16;                                          // seq/stream — not an SE category
}
static float ms_acos(float x) {
    const int i = (int)((x + 1.f) * 50.f);
    return kACosPrm[i < 0 ? 0 : (i > 100 ? 100 : i)];
}
static float calc_volume(float dist, float catDist, u8 swType, u32 cat) {
    if (dist < kMaxVolumeDistance) return 1.f;
    const float d = dist - kMaxVolumeDistance;
    float range = catDist - kMaxVolumeDistance;
    switch (swType) {
    case 1: range = range * 4.f / 3.f; break;
    case 2: range = range * 5.f / 3.f; break;
    case 3: range = range * 2.f;       break;
    case 4: range = range * 3.f / 4.f; break;
    case 5: range = range / 2.f;       break;
    case 6: range = range / 4.f;       break;
    case 7: range = kSeCategory[cat].falloff7; break;
    }
    if (range <= 0.f) return 0.f;
    const float v = 1.f - d / range;                    // JALCalc::linearTransform 1→0
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}
static float calc_pan(float csx, float dist, float catDist) {   // MSHandle::calcPan
    const float amp = kPanMaxAmp;
    const float ac = dist <= 0.f ? 0.f : ms_acos(-csx / dist);
    float v = kPanCAdjust + amp * 2.f * ac / (float)M_PI - amp - kPanCAdjust;
    v = (v < 0.f) ? -amp * powf(-v / amp, kPanCShift)
                  :  amp * powf( v / amp, kPanCShift);
    if (dist < kPanHiSenceDist)
        v *= dist / kPanHiSenceDist;
    else
        v *= (kMSDistMaxSence - 1.f) / (catDist - kPanHiSenceDist)
                 * (dist - kPanHiSenceDist) + 1.f;
    v += amp;
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}
static bool finite_pos(float f) { return f == f && f > -1e8f && f < 1e8f; }
static void se_3d_tick(ActiveSE& a) {                   // MSHandle::setSeDistanceParameters
    if (a.isBgm || !a.posPtr) return;
    const u32 cat = se_cat_of(a.id);
    if (cat >= 16) return;
    const u32 mtx = g_cam.mtx.load(std::memory_order_relaxed);
    if (!mtx) return;
    float w[3], m[12];
    for (int i = 0; i < 3; i++)  w[i] = sb_rf32(a.posPtr + i * 4);
    for (int i = 0; i < 12; i++) m[i] = sb_rf32(mtx + i * 4);
    if (!finite_pos(w[0]) || !finite_pos(w[1]) || !finite_pos(w[2])) return;
    float cs[3];                                        // camera space = ViewMtx × world
    for (int r = 0; r < 3; r++)
        cs[r] = m[r*4]*w[0] + m[r*4+1]*w[1] + m[r*4+2]*w[2] + m[r*4+3];
    const float dist = sqrtf(cs[0]*cs[0] + cs[1]*cs[1] + cs[2]*cs[2]);
    if (!finite_pos(dist)) return;
    const SeCategory& c = kSeCategory[cat];
    const u8 swType = (u8)((a.swbit >> 16) & 7);
    const float vol = (a.swbit & 2) ? 1.f : calc_volume(dist, c.dist, swType, cat);
    a.vol[4].set(vol, c.type);
    const float pan = calc_pan(cs[0], dist, c.dist);
    a.pan[4].set(pan, c.type);
    if (pan != 0.5f || a.pan[4].cur != 0.5f) a.panTouched = true;
    if (a.swbit & 0x10) {                               // random pitch wobble per frame
        const float p = 1.f - (float)(rand() & 0xF) / 192.f;
        a.pitch[4].set(p, c.type);
    }
}

// one JAI frame (60 Hz): advance move params, flush products to worker outer params
static void jai_tick() {
    for (auto& a : g_active) {
        if (!a.used) continue;
        Track* w = a.worker;
        if (a.startAge < 0xFFFF) a.startAge++;
        if (a.isBgm) {
            // BGM root: released when the sequence finishes (track_main -1 in the driver)
            if (!w->used || !w->active) { a.used = false; continue; }
        } else {
            // worker went busy then idle again → sound ended, release the slot
            if (w->portValue[2] != 0) a.seenBusy = true;
            if (a.seenBusy && w->portValue[2] == 0) {
                NJLOG("se_end id=%05x (snippet idle, age=%u)\n", a.id, a.startAge);
                worker_outer_reset(w); a.used = false; continue;
            }
            if (!a.seenBusy && a.startAge > 60) {
                NJLOG("se_end id=%05x (never busy)\n", a.id);
                worker_outer_reset(w); a.used = false; continue;
            }
        }
        if (a.stopFade == 0) { active_stop_now(a); continue; }
        if (a.stopFade > 0) a.stopFade--;
        // lifeTime expiry (JAIEntry::initSoundParameter unk2 = 10): a continuous-class
        // sound no longer re-requested by the game stops after 10 frames (short fade)
        if (!a.isBgm && a.stopFade < 0) {
            const u32 cls = a.id & 0xC00;
            if (cls != 0 && cls != 0x800 && ++a.sinceReq > 10) {
                NJLOG("se_expire id=%05x (no re-request for %u ticks)\n", a.id, a.sinceReq);
                a.vol[6].set(0.f, 6);
                a.stopFade = 6;
            }
        }
        se_3d_tick(a);
        float vp = 1.f, pp = 1.f, pn = 0.5f;
        for (int i = 0; i < 9; i++) {
            a.vol[i].tick(); a.pitch[i].tick(); a.pan[i].tick();
            vp *= a.vol[i].cur;
            pp *= a.pitch[i].cur;
            pn += a.pan[i].cur - 0.5f;
        }
        const u32 cat = (a.id >> 12) & 0xFF;
        w->outerVol = (a.isBgm ? 1.f : g_catvol[cat]) * vp;   // SE categories don't apply to seq
        w->outerPitch = pp;
        w->outerSwitch |= 1 | 2;
        if (a.panTouched) {
            if (pn < 0.f) pn = 0.f; if (pn > 1.f) pn = 1.f;
            w->outerPan = pn;
            w->outerSwitch |= 8;
        }
        w->updateFlags |= 1;             // re-run track_update with new outer params
    }
}

// Data load runs on its own thread: it decodes every wave in the ROM (~1500 AFC
// streams) and MUST NOT block either the emu thread (se_start caller) or the
// audio push path (njas_mix). g_mtx is only ever held for cheap engine work.
static std::atomic<int> g_load_state{0};   // 0 idle, 1 loading, 2 ok, 3 failed

static void engine_start_locked() {        // g_mtx held, data loaded
    init_osc_defaults();
    g_root = track_alloc();
    g_root->trackMode = 3;   // setSeqData root mode
    track_init(*g_root, g_data.seq.data(), 0, nullptr);
    g_root->tickAcc = 0.f;
    g_root->tickPerCall = 1.f;
    track_update(*g_root);
    g_root->active = 1;
    track_update_tempo(*g_root);
    g_players.clear();
    g_players.push_back(g_root);
    g_inited = true;
    fprintf(stderr, "[njas] native JAS engine running (init BMS @0, %zu banks)\n", g_data.banks.size());
}

static bool engine_init() {                // g_mtx held; returns true once running
    if (g_inited) return true;
    int st = g_load_state.load(std::memory_order_acquire);
    if (st == 3) return false;
    if (st == 0 && g_load_state.compare_exchange_strong(st, 1)) {
        std::thread([] {
            const bool ok = load_data();
            g_load_state.store(ok ? 2 : 3, std::memory_order_release);
        }).detach();
        return false;
    }
    if (g_load_state.load(std::memory_order_acquire) != 2) return false;  // still loading
    engine_start_locked();
    return true;
}

// a worker with a bound ActiveSE is claimed even if its exported busy port (port2)
// hasn't updated yet — the BMS only ticks after dispatch, so two requests in one
// pending pass would otherwise both see it "idle" and double-dispatch (the voice-leak
// root cause: the second id overwrites port4, the bindings mismatch, and the guest's
// stop-by-id then releases the wrong slot, leaking looping ambient voices)
static bool worker_claimed(Track* w) {
    for (auto& a : g_active) if (a.used && !a.isBgm && a.worker == w) return true;
    return false;
}

// find a category worker track: exports port9 == cat, idle (export port2 value == 0)
static Track* find_worker(u32 cat, bool* any_for_cat) {
    *any_for_cat = false;
    if (!g_root) return nullptr;
    for (int i = 0; i < 16; i++) {
        Track* mid = g_root->child[i];
        if (!mid) continue;
        for (int j = 0; j < 16; j++) {
            Track* w = mid->child[j];
            if (!w || !w->active) continue;
            if (!w->portExport[9] && w->portValue[9] != cat) continue;
            if (w->portExport[9] && w->portValue[9] == cat) {
                *any_for_cat = true;
                if (w->portValue[2] == 0 && !worker_claimed(w)) return w;   // idle + unclaimed
            }
        }
    }
    return nullptr;
}

// spawn a BGM root playing the BARC entry's BMS (g_mtx held, engine running)
static void bgm_dispatch(u32 id, u8 seqTrack, u8 prio) {
    const u32 idx = id & 0x3FF;
    if (idx >= g_data.barc.size()) return;
    const BarcEntry& e = g_data.barc[idx];
    if (!e.size || e.off + e.size > g_data.seq.size()) return;
    if (active_find(id)) return;   // same BGM already playing → skip (JAIBasic seq gate)
    // One playing BGM per seq-track slot (JAISeqEntry::storeBuffer): a new start stops
    // the incumbent if its info priority allows (lower number = higher priority); this
    // implicit replacement is how e.g. the title music ends when file-select starts —
    // no explicit stop call ever happens.
    for (auto& a : g_active) {
        if (!a.used || !a.isBgm || a.swbit != seqTrack) continue;
        if (a.prio <= prio) { NJLOG("bgm slot %u: %08x replaced by %08x\n", seqTrack, a.id, id); active_stop_now(a); }
        else { NJLOG("bgm slot %u: %08x outranks %08x — start rejected\n", seqTrack, a.id, id); return; }
    }
    Track* root = track_alloc();
    if (!root) return;
    root->trackMode = 3;                  // setSeqData root mode (unk3BC = 3)
    track_init(*root, g_data.seq.data() + e.off, 0, nullptr);
    root->tickAcc = 0.f;
    root->tickPerCall = 1.f;
    track_update(*root);
    root->active = 1;
    track_update_tempo(*root);
    g_players.push_back(root);
    ActiveSE* slot = nullptr;
    for (auto& a : g_active) if (!a.used) { slot = &a; break; }
    if (slot) { active_init(*slot, root, id, seqTrack, prio); slot->isBgm = true; }
    NJLOG("bgm_start id=%08x slot=%u prio=%u → %.14s (off=%#x size=%#x)\n", id, seqTrack,
          prio, e.name, e.off, e.size);
}

static void process_pending() {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        const u32 id = it->id;
        const u32 cat = (id >> 12) & 0xFF;
        if (it->isBgm) {
            bgm_dispatch(id, (u8)it->swbit, it->prio);   // swbit reused = seq track slot
            it = g_pending.erase(it);
            continue;
        }
        // Request lifecycle (JAISeEntry::storeBuffer): a re-request of a CONTINUOUS-class
        // sound (id & 0xC00 set, except the 0x800 restart class) with the same id+actor
        // REVIVES the playing instance — ambient keep-alives (e.g. the plaza festival
        // drums, id 0x500x) are re-requested every frame and must NOT restart. Restart
        // class / different actor / one-shots: stop the old instance (unless swbit
        // bit19 allows stacking).
        // Instance identity is (id, actor) — JAISeEntry::storeBuffer matches BOTH the id
        // and the actor position pointer; the same SE id sounding from several actors is
        // several concurrent sounds. Keying by id alone made every other-actor request
        // stomp+restart the instance (4k restarts/170 s of one ambient id — churn that
        // also starved the category's workers, silencing e.g. the water spray).
        {
            ActiveSE* same = nullptr;       // same id + same actor
            int idCount = 0; ActiveSE* oldest = nullptr;
            for (auto& a : g_active) {
                if (!a.used || a.isBgm || a.id != id) continue;
                idCount++;
                if (a.posPtr == it->posPtr) same = &a;
                if (!oldest || a.startAge > oldest->startAge) oldest = &a;
            }
            if (same && !(it->swbit & 0x80000)) {
                const u32 cls = id & 0xC00;
                const bool alive = same->stopFade < 0 && same->worker
                                   && same->worker->portValue[2] != 0;
                if (cls != 0 && cls != 0x800 && alive) {
                    same->sinceReq = 0;               // keep-alive refresh, no restart
                    it = g_pending.erase(it);
                    continue;
                }
                active_stop_now(*same);               // restart class / finished → fresh
            } else if (idCount >= 4 && oldest) {
                active_stop_now(*oldest);             // per-id concurrency cap (steal oldest)
            }
        }
        bool any = false;
        Track* w = find_worker(cat, &any);
        if (w) {
            ActiveSE* slot = nullptr;
            for (auto& a : g_active) if (!a.used) { slot = &a; break; }
            if (!slot) { ++it; continue; }   // registry full — wait (binding is mandatory)
            active_init(*slot, w, id, it->swbit, it->prio);
            slot->posPtr = it->posPtr;
            w->writePortImport(4, (u16)(id & 0x3FF));
            w->writePortImport(0, 1);
            NJLOG("se_start id=%05x cat=%u idx=%u pos=%08x → worker %p\n", id, cat, id & 0x3FF,
                  it->posPtr, (void*)w);
            it = g_pending.erase(it);
        } else if (g_inited && ++it->age > 800) {
            // expire stale requests (~2 s of subframes with no worker): all busy or unknown category
            NJLOG("se_start id=%05x expired (no idle worker, any_for_cat=%d)\n", id, any);
            it = g_pending.erase(it);
        } else {
            ++it;   // workers not opened yet (init BMS booting) or all busy — wait
        }
    }
}

// one subframe: tick sequencer + render kSub frames into g_mixbuf
static void render_subframe() {
    process_pending();
    // JAI frame clock: handle/move-param flush runs at 60 Hz (subframe rate ≈ 400.36 Hz)
    static float jai_acc = 0.f;
    jai_acc += 60.f * kSub / kDacRate;
    while (jai_acc >= 1.f) { jai_acc -= 1.f; jai_tick(); }
    for (auto it = g_players.begin(); it != g_players.end();) {
        Track* root = *it;
        bool finished = !root->used || !root->active;
        if (!finished) {
            root->tickAcc += root->tickPerCall;
            if (root->tickAcc > 1.f) {
                track_update_seq(*root, 0, true);
                while (root->tickAcc >= 1.f) {
                    root->tickAcc -= 1.f;
                    if (track_main(*root) == -1) { finished = true; break; }
                }
            }
            if (!finished) track_inc_self_osc(*root);
        }
        if (finished) {
            if (root != g_root) { track_close_recursive(*root); it = g_players.erase(it); continue; }
            root->active = 0;
        }
        ++it;
    }

    memset(g_mixbuf, 0, sizeof(g_mixbuf));
    for (auto& v : g_voices) {
        if (!v.active) continue;
        v.age++;
        Track& t = *v.owner;
        // per-update voice maintenance (updatecallDSPChannel param==0 path)
        v.oscPitch = 1.f; v.oscVol = 1.f;
        bool ended = false;
        for (int i = 0; i < 2; i++) {
            if (!v.oscs[i].isOsc()) continue;
            float off = v.oscs[i].getOffset();
            switch (v.oscs[i].osc->mode) {
            case 1: v.oscPitch *= off; break;
            case 0: v.oscVol *= off; break;
            default: break;
            }
            if (i == 0 && v.oscs[i].state == 0) ended = true;
        }
        if (ended) { v.stop_now(); continue; }
        if (v.gateLeft > 0) {
            if (v.gateLeft > 1) v.gateLeft -= 1;
            else {
                v.gateLeft = 0;
                // gate end → release
                if (v.oscs[0].isOsc()) v.oscs[0].release(); else { v.stop_now(); continue; }
                if (v.oscs[1].isOsc()) v.oscs[1].release();
            }
        }
        if (v.pause || t.paused) continue;

        const float ratio = t.chPitch * v.pitchRatio * v.oscPitch;
        float vol = t.chVol * v.vol * v.oscVol;
        v.lastRatio = ratio; v.lastVol = vol;
        v.lastChVol = t.chVol; v.lastOscVol = v.oscVol;
        if (vol < 0.f) vol = 0.f; if (vol > 1.f) vol = 1.f;
        float pan = t.chPan + (v.panSound - 0.5f);   // CALC_ADD combine (JASChannel calcPan)
        if (pan < 0.f) pan = 0.f; if (pan > 1.f) pan = 1.f;
        const float gl = vol * sinf((1.f - pan) * (float)M_PI / 2.f);
        const float gr = vol * sinf(pan * (float)M_PI / 2.f);
        const std::vector<s16>& pcm = v.wave->pcm;
        const size_t n = pcm.size();
        const bool loop = v.wave->loop != 0;
        const double le = (loop && v.wave->loop_e && v.wave->loop_e <= n) ? v.wave->loop_e : (double)n;
        const double ls = (loop && v.wave->loop_s < le) ? v.wave->loop_s : 0.0;
        double pos = v.srcpos;
        for (int i = 0; i < kSub; i++) {
            if (pos >= le - 1) {
                if (loop) pos = ls + (pos - (le - 1));
                else { v.stop_now(); break; }
            }
            const size_t i0 = (size_t)pos;
            const float fr = (float)(pos - i0);
            const float s = pcm[i0] * (1.f - fr) + pcm[i0 + 1 < n ? i0 + 1 : i0] * fr;
            g_mixbuf[i * 2 + 0] += s * gl;
            g_mixbuf[i * 2 + 1] += s * gr;
            pos += ratio;
        }
        v.srcpos = pos;
    }
    if (FILE* ab = ab_out()) {                 // A/B: peak tracking + voice-off sweep
        for (int i = 0; i < kMaxVoices; i++) {
            Voice& v = g_voices[i];
            if (!v.abOpen) continue;
            if (v.active) {
                if (v.lastVol > v.abPeak) v.abPeak = v.lastVol;
            } else {
                v.abOpen = false;
                fprintf(ab, "{\"ev\":\"voff\",\"t\":%.1f,\"hash\":\"%08x\",\"dur\":%u,"
                            "\"peak\":%.4f,\"id\":\"%08x\",\"seqoff\":\"%x\"}\n",
                        ab_now_ms(), v.wave ? v.wave->srcHash : 0, v.age, v.abPeak,
                        v.abSoundId, v.abSeqOff);
                fflush(ab);
            }
        }
    }
    g_mix_have = kSub; g_mix_pos = 0;

    if (g_solo_dump) {
        s16 tmp[kSub * 2];
        for (int i = 0; i < kSub * 2; i++) {
            float s = g_mixbuf[i];
            if (s > 32767.f) s = 32767.f; if (s < -32768.f) s = -32768.f;
            tmp[i] = (s16)s;
        }
        fwrite(tmp, sizeof(s16), kSub * 2, g_solo_dump);
    }
}

}  // namespace njas

// ============================ public API ============================

// Mix `frames` stereo frames of native JAS output into `buf` (host-order interleaved LR
// s16) — called from na_push_dsp with the converted guest DSP stream. Rates are treated
// as equal (guest 32028 vs JAS 32028.5 — 0.0016% apart, inaudible).
extern "C" void njas_mix(int16_t* buf, size_t frames) {
    using namespace njas;
    std::unique_lock<std::mutex> lk(g_mtx, std::try_to_lock);
    if (!lk.owns_lock()) return;          // never stall the audio path
    if (!g_inited) {
        // finish engine start once the async data load completes
        if (g_load_state.load(std::memory_order_acquire) != 2 || !engine_init()) return;
    }
    static bool dump_checked = false;
    if (!dump_checked) {
        dump_checked = true;
        const char* e = getenv("SUNBRIGHT_DUMP_NJAS");
        if (e && *e && *e != '0') {
            g_solo_dump = fopen("scratch/wav/njas_solo.raw", "wb");
            fprintf(stderr, "[njas] dumping engine-only mix to scratch/wav/njas_solo.raw (s16 LR @32028)\n");
        }
    }
    for (size_t i = 0; i < frames; i++) {
        if (g_mix_pos >= g_mix_have) render_subframe();
        int l = buf[i * 2 + 0] + (int)g_mixbuf[g_mix_pos * 2 + 0];
        int r = buf[i * 2 + 1] + (int)g_mixbuf[g_mix_pos * 2 + 1];
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        buf[i * 2 + 0] = (int16_t)l;
        buf[i * 2 + 1] = (int16_t)r;
        g_mix_pos++;
    }
}

// Start an SE natively (called from the startSoundBasic tee). SE ids only (top bits 0);
// seq/stream ids are ignored here. swbit/prio come from the guest JAISoundInfo.
extern "C" void njas_se_start(uint32_t id, uint32_t swbit, uint8_t prio, uint32_t posPtr) {
    using namespace njas;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_catvol_init) { for (auto& v : g_catvol) v = 1.f; g_catvol_init = true; }
    engine_init();                            // kicks the async data load on first call
    PendingSE p{ id, swbit, prio };
    p.posPtr = posPtr;                        // game-world Vec* (0 = no 3D / UI sound)
    g_pending.push_back(p);                   // queued until the engine is running
}

// Camera input for the native 3D layer (JAIBasic::setCameraInfo tee): the game hands
// JAI its camera position/view-matrix POINTERS once per camera; we keep the same.
extern "C" void njas_set_camera(uint32_t posPtr, uint32_t mtxPtr) {
    using namespace njas;
    g_cam.pos.store(posPtr, std::memory_order_relaxed);
    g_cam.mtx.store(mtxPtr, std::memory_order_relaxed);
}

// Start a BGM natively (seq-class id 0x8xxxxxxx from the startSoundBasic tee): spawn a
// new root track playing the BARC entry's BMS (id & 0x3FF indexes the BARC table; the
// offsets point into sequence.arc, which is loaded whole). Entry 0 = the init/SE BMS
// already running as g_root — ignored. All banks/waves are resident natively, so the
// guest's wScene/ARAM residency dance is unnecessary.
extern "C" void njas_bgm_start(uint32_t id, uint8_t seqTrack, uint8_t prio) {
    using namespace njas;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_catvol_init) { for (auto& v : g_catvol) v = 1.f; g_catvol_init = true; }
    engine_init();                              // kicks the async data load on first call
    if ((id & 0x3FF) == 0) return;              // entry 0 = the init/SE BMS (always running)
    g_pending.push_back({ id, seqTrack, prio, true });   // swbit slot reused = seq track
}

// Stop a playing SE (JAIBasic::stopSoundHandle tee). fade = frames to fade out (0 = now).
// posPtr disambiguates the instance (identity = id+actor); 0 / no match → stop all of id.
extern "C" void njas_se_stop(uint32_t id, uint32_t fade, uint32_t posPtr) {
    using namespace njas;
    std::lock_guard<std::mutex> lk(g_mtx);
    // also drop any not-yet-dispatched request for this instance
    for (auto it = g_pending.begin(); it != g_pending.end();)
        it = (it->id == id && (!posPtr || it->posPtr == posPtr)) ? g_pending.erase(it) : it + 1;
    bool matched = false;
    for (int pass = 0; pass < 2 && !matched; pass++) {
        for (auto& a : g_active) {
            if (!a.used || a.id != id) continue;
            if (pass == 0 && posPtr && a.posPtr != posPtr) continue;   // exact instance first
            matched = true;
            if (fade == 0) { active_stop_now(a); continue; }
            a.vol[6].set(0.f, fade);          // stopSoundHandle: setSeInterVolume(6, 0, fade)
            a.stopFade = (s32)fade;
        }
        if (!posPtr) break;
    }
    NJLOG("se_stop id=%05x fade=%u pos=%08x matched=%d\n", id, fade, posPtr, (int)matched);
}

// Handle param op (JAISound::setVolume/setPan/setPitch tees). kind: 0=vol 1=pan 2=pitch.
extern "C" void njas_se_param(uint32_t id, int kind, uint8_t slot, float value, uint32_t time) {
    using namespace njas;
    std::lock_guard<std::mutex> lk(g_mtx);
    ActiveSE* a = active_find(id);
    if (!a || slot >= 9) return;
    switch (kind) {
    case 0: a->vol[slot].set(value, time); break;
    case 1: a->pan[slot].set(value, time); a->panTouched = true; break;
    case 2: a->pitch[slot].set(value, time); break;
    }
    NJLOG("se_param id=%05x kind=%d slot=%u v=%.3f t=%u\n", id, kind, slot, value, time);
}

// Probe dump of the native voice table (probe_server /njas): one line per active voice
// with the oracle-comparison join key (srcHash = FNV of the wave's first 64 raw .aw
// bytes — the same bytes /aram hashes at a VPB's base address) plus the effective
// resampling ratio in the VPB's 4.12 fixed-point and the pre-pan voice volume.
extern "C" int njas_probe(char* out, int cap) {
    using namespace njas;
    std::unique_lock<std::mutex> lk(g_mtx, std::try_to_lock);
    int n = 0;
    auto app = [&](const char* fmt, auto... a) {
        if (n < cap) n += snprintf(out + n, (size_t)(cap - n), fmt, a...);
    };
    if (!lk.owns_lock()) { app("busy\n"); return n; }
    app("inited=%d players=%zu scene0=%d scene1=%d scene2=%d\n", (int)g_inited,
        g_players.size(), g_wave_scene[0].load(), g_wave_scene[1].load(), g_wave_scene[2].load());
    for (auto& a : g_active) {
        if (!a.used || a.isBgm) continue;
        float vp = 1.f; for (int i = 0; i < 9; i++) vp *= a.vol[i].cur;
        app("a id=%05x pos=%08x volprod=%.4f outer=%.4f vol4=%.3f pan4=%.3f\n", a.id,
            a.posPtr, vp, a.worker ? a.worker->outerVol : -1.f, a.vol[4].cur, a.pan[4].cur);
    }
    for (int i = 0; i < kMaxVoices; i++) {
        Voice& v = g_voices[i];
        if (!v.active || !v.wave) continue;
        app("v%02d hash=%08x wsys=%d grp=%d wave=%u ratio=%04x vol=%.4f key=%u vel=%u bank=%u prog=%u st=%u age=%u chvol=%.4f notevol=%.4f oscvol=%.4f\n",
            i, v.wave->srcHash, (int)v.wave->wsysIdx, (int)v.wave->groupIdx, v.wave->id,
            (unsigned)(v.lastRatio * 4096.f + 0.5f), v.lastVol,
            v.key, v.vel, v.bankProg >> 8, v.bankProg & 0xFF, v.oscs[0].state, v.age,
            v.lastChVol, v.vol, v.lastOscVol);
    }
    return n;
}

// Resident wave-scene selection (JAIBasic::loadGroupWave tee): the guest loads scene
// group `scene` for wave bank `wsys` (on hardware: into ARAM). Natively all groups are
// pre-decoded — we just switch the active table used by future noteOns.
extern "C" void njas_set_wave_scene(int wsys, int scene) {
    using namespace njas;
    if (wsys < 0 || wsys >= 8) return;
    g_wave_scene[wsys].store(scene, std::memory_order_relaxed);
    NJLOG("wave scene: wsys %d → group %d\n", wsys, scene);
}

// Category master volume (JAIBasic::setSeCategoryVolume tee), vol = 0..127.
extern "C" void njas_se_category_volume(uint8_t cat, uint8_t vol) {
    using namespace njas;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_catvol_init) { for (auto& v : g_catvol) v = 1.f; g_catvol_init = true; }
    g_catvol[cat] = vol / 127.f;
    NJLOG("se_catvol cat=%u vol=%u\n", cat, vol);
}
