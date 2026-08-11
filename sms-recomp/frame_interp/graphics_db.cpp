// graphics_db.cpp — the graphics registry: detection, measurement, and the file that outlives the
// run. Read graphics_db.h first; it states what this can and cannot see.
//
// THREE THINGS HAPPEN HERE, and keeping them apart is what makes the file trustworthy:
//
//   DETECTION is automatic and costs a hash lookup at the GX waists. A guest address that emits
//   geometry gets a row the first time it does so. Nothing has to be added to a list.
//
//   MEASUREMENT is aurora's interpolation audit, read back per population at flush time. It is
//   only populated when interpolation is running, so a row written by a non-interpolated run says
//   `unmeasured` — which is a different fact from `snaps` and must never be written as one.
//
//   CURATION is human (or agent) input — the `re` verdict and the note — and this file NEVER
//   overwrites it. A row's measured columns are rewritten every run; its curated columns are read
//   from the existing file and written back unchanged. That is the whole reason the registry can
//   answer "did someone decide this snaps, or has nobody looked".

#include "graphics_db.h"

#include "populations.h"

#include <lucent/log.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// aurora's audit, declared rather than included — aurora's gfx headers are internal to the library
// and the recomp links it statically, the same approach the rest of this directory uses.
namespace aurora::gfx::interp {
void audit_row(uint8_t pop, long* out, int outLen);
void name_population(uint8_t pop, const char* name);
int max_populations();
} // namespace aurora::gfx::interp

// Does a native override exist for this guest function? A non-announcing query on purpose: asking
// the registry a question must not make the override registry log "-> native", which is a
// statement about what the game EXECUTED.
bool override_exists(u32 address);

namespace {

// Disposition indices, matching aurora::gfx::interp::Disposition. Plain ints rather than the enum
// because that enum lives in an internal aurora header; the ORDER is the contract and audit_row()
// fills by index.
enum : int { kUnclaimed = 0, kPaired, kBillboard, kCameraOnly, kSnapOrtho, kSnapNoId, kDispCount };

// The curated populations occupy the low ids (populations.h). Sites are allocated from here up, so
// a hand-written label and an auto-detected site can never collide.
constexpr int kFirstSiteId = 16;
constexpr int kPopCount = 256;

bool enabled_impl() {
    const char* v = std::getenv("SBR_GFXDB");
    return v == nullptr || (v[0] != '0' && v[0] != '\0');
}

struct Site {
    u32 addr = 0;
    SbGfxWaist waist = SbGfxWaist::Immediate;
    unsigned long calls = 0;   // counted at the waist THIS run
    bool used = false;
    // Kept alive here because aurora's audit stores the pointer's CONTENTS at registration but the
    // report prints per run; more to the point, a row printed as "pop 37" is a row nobody can act
    // on, and the log and the registry file must name the same thing.
    std::string name;
};

Site g_sites[kPopCount];
std::unordered_map<u32, u8> g_byAddr;
int g_nextId = kFirstSiteId;
std::unordered_set<u32> g_overflowSites;   // distinct addresses that found no free id
unsigned long g_overflowCalls = 0;
bool g_countedRun[kPopCount] = {};         // this key's `runs` already incremented this run
unsigned long g_newThisRun = 0;

// ── SYMBOLS ─────────────────────────────────────────────────────────────────────────────────────
//
// A registry of bare addresses is a registry nobody reads. Names come from the same US function
// list every diagnostic in this project resolves against (tools/re/addr2sym.py). If it cannot be
// found the rows are still written — with `?` and a LOUD one-time warning, because a file full of
// unnamed rows that does not say why looks like a broken game rather than a missing text file.
struct Sym {
    u32 addr;
    std::string name;
};
std::vector<Sym> g_syms;
bool g_symsTried = false;
bool g_symsOk = false;

std::string repo_root() {
    // Walk up from the working directory looking for the repo's own marker. The run scripts launch
    // from the repo root, so this normally terminates immediately; the walk exists so a run started
    // from a build directory still finds it.
    static std::string cached;
    static bool done = false;
    if (done) return cached;
    done = true;
    char buf[4096];
    if (getcwd(buf, sizeof(buf)) == nullptr) return cached;
    std::string p(buf);
    for (int up = 0; up < 8; ++up) {
        if (FILE* f = std::fopen((p + "/CLAUDE.md").c_str(), "rb")) {
            std::fclose(f);
            cached = p;
            return cached;
        }
        const size_t slash = p.find_last_of('/');
        if (slash == std::string::npos || slash == 0) break;
        p.resize(slash);
    }
    return cached;
}

void load_symbols() {
    if (g_symsTried) return;
    g_symsTried = true;
    const std::string root = repo_root();
    const std::string path = root.empty() ? std::string() : root + "/reference/sms_gmse01_funcs.txt";
    FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        lucent::warn("gfxdb", "the US function list ({}) could not be opened, so every row in the "
                              "graphics registry is written with symbol '?'. The addresses are "
                              "still correct — resolve them with tools/re/addr2sym.py — but the "
                              "rows are unnamed because a FILE is missing, not because the "
                              "emitters are unknown.",
                     path.empty() ? "<repo root not found>" : path.c_str());
        return;
    }
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char name[256] = {};
        unsigned addr = 0;
        if (std::sscanf(line, "%x %255s", &addr, name) == 2 && addr != 0) {
            g_syms.push_back({(u32)addr, std::string(name)});
        }
    }
    std::fclose(f);
    std::sort(g_syms.begin(), g_syms.end(),
              [](const Sym& a, const Sym& b) { return a.addr < b.addr; });
    g_symsOk = !g_syms.empty();
    if (!g_symsOk) {
        lucent::warn("gfxdb", "the US function list parsed to ZERO symbols — every row is '?'.");
    }
}

// Beyond this an address is not inside the preceding function, it is past the end of it. The same
// bound addr2sym.py uses, and for the same reason: attributing a return address to the wrong
// function is worse than leaving it unnamed.
constexpr u32 kMaxSymOffset = 0x10000;

const Sym* containing(u32 addr) {
    load_symbols();
    if (!g_symsOk) return nullptr;
    auto it = std::upper_bound(g_syms.begin(), g_syms.end(), addr,
                               [](u32 a, const Sym& s) { return a < s.addr; });
    if (it == g_syms.begin()) return nullptr;
    --it;
    return (addr - it->addr) <= kMaxSymOffset ? &*it : nullptr;
}

// A large offset is a WARNING, not a name. The US function list is known to be incomplete (weak
// virtual methods are absent from it entirely), so an address that falls in one of its gaps
// resolves to whatever function happens to precede the gap, plus a big offset — which reads as a
// confident answer and is a wrong one. Past this, the name is marked `?` so nobody builds on it.
// The bound is the size of the largest functions that really exist here; addr2sym.py's much looser
// 0x10000 is a search bound, not a confidence bound.
constexpr u32 kTrustedSymOffset = 0x1000;

std::string symbolize(u32 addr) {
    const Sym* s = containing(addr);
    if (s == nullptr) return "?";
    const u32 off = addr - s->addr;
    if (off == 0) return s->name;
    char b[32];
    std::snprintf(b, sizeof(b), "+0x%x%s", off, off > kTrustedSymOffset ? "?" : "");
    return s->name + b;
}

const char* waist_name(SbGfxWaist w) {
    switch (w) {
    case SbGfxWaist::Indexed: return "indexed";
    case SbGfxWaist::Immediate: return "immediate";
    case SbGfxWaist::J3DShape: return "j3d-shape";
    }
    return "?";
}

// ── THE CURATED POPULATIONS ─────────────────────────────────────────────────────────────────────
//
// The eleven hand-written labels get rows too, under stable slugs rather than addresses. They live
// in the same table as the auto-detected sites because "what graphics are there" has one answer: a
// reader must not have to consult two lists and work out which one covers what.
struct Curated {
    u8 pop;
    const char* slug;
    const char* kind;
};
constexpr Curated kCurated[] = {
    {SB_POP_UNLABELLED, "pop.unlabelled", "audit-edge"},
    {SB_POP_J3D_SHAPE, "pop.j3d-shape", "world"},
    {SB_POP_SHADOW_VOLUME, "pop.shadow-volume", "shadow"},
    {SB_POP_SHADOW_SHINE, "pop.shine-shadow-slice", "shadow"},
    {SB_POP_SHADOW_MODEL, "pop.shadow-model", "shadow"},
    {SB_POP_PARTICLE, "pop.jpa-particle", "effect"},
    {SB_POP_FLAG, "pop.flag", "deforming"},
    {SB_POP_WAVE, "pop.sea-ripple", "deforming"},
    {SB_POP_DRAW_CUBE, "pop.shadow-alpha-cube", "shadow"},
    {SB_POP_TEXT, "pop.text-glyph", "2d"},
    {SB_POP_J2D, "pop.j2d-pane", "2d"},
};

// ── THE FILE ────────────────────────────────────────────────────────────────────────────────────
//
// Tab-separated because it must be BOTH machine-mergeable (this file re-reads it every flush to
// preserve curation) and readable in a diff — a JSON blob rewritten every run produces a diff
// nobody reviews, which is how a tracked file stops being read at all.
constexpr int kCols = 13;
constexpr const char* kHeader =
    "key\tkind\tsymbol\tstages\tre\tlerp\tinterp_pct\tdraws\tcalls\truns\tfirst_seen\tlast_seen\tnote";

struct Entry {
    std::string key, kind, symbol, stages, re, lerp, pct, draws, calls, first, last, note;
    long runs = 0;
};

std::string db_path() {
    if (const char* p = std::getenv("SBR_GFXDB_PATH")) return p;
    const std::string root = repo_root();
    if (root.empty()) return std::string();
    return root + "/docs/graphics/graphics_db.tsv";
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char b[16];
    std::strftime(b, sizeof(b), "%Y-%m-%d", &tmv);
    return b;
}

// Where this run was. A registry that cannot say WHERE a graphic was seen cannot tell "absent from
// this stage" from "never observed anywhere", which is the distinction the file exists for.
std::string stage_label() {
    if (const char* s = std::getenv("SBR_STAGE")) return std::string("stage") + s;
    if (const char* s = std::getenv("SB_STAGE")) return std::string("stage") + s;
    if (std::getenv("SBR_FASTBOOT")) return "plaza";
    return "boot";
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t at = s.find(sep, start);
        out.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return out;
}

// A tab or a newline inside a curated note would silently split one row into two on the next read.
// Losing the exact whitespace of a note costs nothing; a registry that corrupts itself when
// somebody types a tab is one nobody can trust.
std::string sanitize(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return out.empty() ? "-" : out;
}

std::string join_stages(const std::string& existing, const std::string& add) {
    std::vector<std::string> parts;
    for (const std::string& p : split(existing, ',')) {
        if (!p.empty() && p != "-") parts.push_back(p);
    }
    if (std::find(parts.begin(), parts.end(), add) == parts.end()) parts.push_back(add);
    std::sort(parts.begin(), parts.end());
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out += ',';
        out += parts[i];
    }
    return out.empty() ? "-" : out;
}

std::string num(long v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%ld", v);
    return b;
}

// The measured verdict, from one population's audit row. Deliberately mirrors aurora's report_audit
// wording so the log and the file cannot disagree about the same numbers.
//
// `unmeasured` is NOT a verdict about the graphic — it means the audit filed nothing for it, which
// happens when the run had interpolation off, or when the emitter was detected but drew nothing
// while the audit was live. Writing that as "no" would manufacture a defect out of a missing
// measurement, and this registry's whole value is that those two stay apart.
void verdict(const long* row, bool auditLive, std::string& lerp, std::string& pct, long& draws) {
    draws = 0;
    for (int i = 0; i < kDispCount; ++i) draws += row[i];
    if (draws == 0) {
        // Two different facts, and collapsing them is exactly the kind of silence this registry
        // exists to prevent. If the audit classified draws for OTHER populations this run then it
        // was live, and this emitter genuinely produced no primitives — a material display list, or
        // a call that set state and drew nothing. If the audit filed nothing at all, nothing was
        // measured and the row must not imply otherwise.
        lerp = auditLive ? "no-primitives" : "unmeasured";
        pct = "-";
        return;
    }
    const long good = row[kPaired] + row[kBillboard];
    const long noId = row[kSnapNoId] + row[kUnclaimed];
    const long bad = row[kCameraOnly] + noId;
    if (bad == 0 && good == 0) {
        lerp = "2d-correct";        // screen-space: there IS no in-between, so snapping is right
        pct = "-";
        return;
    }
    lerp = bad == 0 ? "yes" : good == 0 ? (noId == 0 ? "camera-only" : "no") : "partial";
    char b[16];
    std::snprintf(b, sizeof(b), "%.1f", 100.0 * (double)good / (double)(good + bad));
    pct = b;
}

bool g_warnedNoPath = false;

std::vector<Entry> read_db(const std::string& path) {
    std::vector<Entry> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return out;
    std::string buf;
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) buf.append(chunk, n);
    std::fclose(f);
    for (const std::string& line : split(buf, '\n')) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> c = split(line, '\t');
        // A short line is a row this build cannot understand — a column added by a newer version,
        // or a hand edit that broke the shape. Dropping it silently would delete somebody's
        // curation, so it is kept verbatim by refusing to parse the file at all.
        if ((int)c.size() != kCols) {
            if (c[0] == "key") continue;   // the header
            lucent::error("gfxdb", "{} has a row with {} column(s), expected {}: '{}'. REFUSING to "
                                   "rewrite the file — a rewrite would drop the curated columns of "
                                   "every row it could not parse.",
                          path, c.size(), kCols, line.substr(0, 120));
            out.clear();
            out.push_back(Entry{});   // marker: parse failed (key empty)
            return out;
        }
        Entry e;
        e.key = c[0]; e.kind = c[1]; e.symbol = c[2]; e.stages = c[3]; e.re = c[4];
        e.lerp = c[5]; e.pct = c[6]; e.draws = c[7]; e.calls = c[8];
        e.runs = std::strtol(c[9].c_str(), nullptr, 10);
        e.first = c[10]; e.last = c[11]; e.note = c[12];
        if (!e.key.empty() && e.key != "key") out.push_back(e);
    }
    return out;
}

} // namespace

bool sbr_gfxdb_enabled() {
    static const bool v = enabled_impl();
    return v;
}

u8 sbr_gfxdb_site(u32 guestAddr, SbGfxWaist waist) {
    if (!sbr_gfxdb_enabled() || guestAddr == 0) return SB_POP_UNLABELLED;
    const auto it = g_byAddr.find(guestAddr);
    if (it != g_byAddr.end()) {
        ++g_sites[it->second].calls;
        return it->second;
    }
    if (g_nextId >= kPopCount) {
        // LOUD, and counted separately. Folding these into row 0 would put draws whose emitter IS
        // known under the label that means "no seam claims this", which is the one thing the audit's
        // edge bucket must never contain.
        if (g_overflowSites.insert(guestAddr).second) {
            lucent::warn("gfxdb", "population id space EXHAUSTED at {} sites; 0x{:08x} ({}) and any "
                                  "further emitter get no row of their own this run. They are "
                                  "counted as overflow, not attributed.",
                         kPopCount - kFirstSiteId, guestAddr, symbolize(guestAddr));
        }
        ++g_overflowCalls;
        return SB_POP_UNLABELLED;
    }
    const u8 id = (u8)g_nextId++;
    g_sites[id] = Site{guestAddr, waist, 1, true, symbolize(guestAddr)};
    g_byAddr[guestAddr] = id;
    aurora::gfx::interp::name_population(id, g_sites[id].name.c_str());
    ++g_newThisRun;
    return id;
}

void sbr_gfxdb_flush() {
    if (!sbr_gfxdb_enabled()) return;
    const std::string path = db_path();
    if (path.empty()) {
        if (!g_warnedNoPath) {
            g_warnedNoPath = true;
            lucent::warn("gfxdb", "no repo root found from the working directory, so the graphics "
                                  "registry has nowhere to persist. Detection still runs and the "
                                  "run report below is complete; nothing is written. Set "
                                  "SBR_GFXDB_PATH=<file> to choose the location explicitly.");
        }
        return;
    }

    std::vector<Entry> db = read_db(path);
    if (db.size() == 1 && db[0].key.empty()) return;   // unparseable: read_db already refused

    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < db.size(); ++i) index[db[i].key] = i;

    const std::string stamp = today();
    const std::string stage = stage_label();

    // Did the audit classify ANYTHING this run? This is the denominator that separates "this
    // emitter drew no primitives" from "nothing was measured at all", and without it both are a
    // zero row that reads as the former.
    bool auditLive = false;
    for (int p = 0; p < kPopCount && !auditLive; ++p) {
        long row[kDispCount];
        aurora::gfx::interp::audit_row((u8)p, row, kDispCount);
        for (int d = 0; d < kDispCount; ++d) {
            if (row[d] != 0) { auditLive = true; break; }
        }
    }

    auto upsert = [&](const std::string& key, const std::string& kind, const std::string& symbol,
                      u8 pop, unsigned long calls, const char* seedRe) {
        long row[kDispCount];
        aurora::gfx::interp::audit_row(pop, row, kDispCount);
        std::string lerp, pct;
        long draws = 0;
        verdict(row, auditLive, lerp, pct, draws);

        // A curated population that neither drew nor was counted is one this run never reached.
        // Writing a row for it would say "observed", so it is skipped — and its EXISTING row, if it
        // has one, is left untouched rather than being overwritten with this run's zeros.
        if (draws == 0 && calls == 0) return;

        const auto at = index.find(key);
        const bool fresh = at == index.end();
        size_t idx;
        if (fresh) {
            db.push_back(Entry{});
            idx = db.size() - 1;
            index[key] = idx;
        } else {
            idx = at->second;
        }
        Entry& e = db[idx];
        if (fresh) {
            e.key = key;
            e.first = stamp;
            e.re = seedRe;      // the ONLY time `re` is written by the game
            e.note = "-";
            e.runs = 0;
        }
        e.kind = kind;
        e.symbol = symbol;
        e.stages = join_stages(e.stages, stage);
        e.lerp = lerp;
        e.pct = pct;
        e.draws = num(draws);
        e.calls = num((long)calls);
        e.last = stamp;
        if (!g_countedRun[pop]) {
            g_countedRun[pop] = true;   // periodic flushes must not inflate the run count
            ++e.runs;
        }
    };

    for (const Curated& c : kCurated) {
        upsert(c.slug, c.kind, "(labelled by a seam in sms-recomp/frame_interp)", c.pop, 0,
               "identified");
    }
    for (int id = kFirstSiteId; id < kPopCount; ++id) {
        if (!g_sites[id].used) continue;
        const Site& s = g_sites[id];
        char key[16];
        std::snprintf(key, sizeof(key), "0x%08x", s.addr);
        // The seed is a HINT, not a verdict: it says a native override exists for the function that
        // emitted this, which is evidence someone has been here. Everything else starts `unknown`,
        // and only a human or an agent editing the file turns that into an answer.
        const Sym* fn = containing(s.addr);
        const char* seed = (fn != nullptr && override_exists(fn->addr)) ? "native-override" : "unknown";
        upsert(key, waist_name(s.waist), symbolize(s.addr), (u8)id, s.calls, seed);
    }

    std::sort(db.begin(), db.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });

    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        lucent::warn("gfxdb", "cannot write {} — the registry is not being persisted this run.", tmp);
        return;
    }
    std::fprintf(f, "# THE GRAPHICS REGISTRY — every graphic this port has been OBSERVED to draw.\n");
    std::fprintf(f, "# Written by the game itself (sms-recomp/frame_interp/graphics_db.cpp). Rows "
                    "are added automatically the first\n# frame an emitter draws; measured columns "
                    "are rewritten every run; the CURATED columns `re` and `note` are\n# never "
                    "touched by the game. Edit those with tools/gfx/graphics_db.py.\n");
    std::fprintf(f, "#\n# WHAT THIS FILE DOES NOT SAY: a graphic that has never drawn in a "
                    "recorded run is ABSENT, not \"does not\n# exist\" — this is a census of what "
                    "was observed. `lerp=unmeasured` means the interpolation audit filed\n# "
                    "nothing (the run had SBR_LERP60 off), NOT that the graphic snaps.\n");
    std::fprintf(f, "#\n# re:   unknown | native-override (auto hints) | yes | partial | no | "
                    "identified   (curated verdicts)\n");
    std::fprintf(f, "# lerp: yes | partial | camera-only | no | 2d-correct | unmeasured   "
                    "(MEASURED, per run)\n");
    std::fprintf(f, "%s\n", kHeader);
    for (const Entry& e : db) {
        std::fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%ld\t%s\t%s\t%s\n",
                     e.key.c_str(), sanitize(e.kind).c_str(), sanitize(e.symbol).c_str(),
                     sanitize(e.stages).c_str(), sanitize(e.re).c_str(), sanitize(e.lerp).c_str(),
                     sanitize(e.pct).c_str(), sanitize(e.draws).c_str(), sanitize(e.calls).c_str(),
                     e.runs, sanitize(e.first).c_str(), sanitize(e.last).c_str(),
                     sanitize(e.note).c_str());
    }
    std::fclose(f);
    // Atomic replace: a run killed mid-write (which is how automated runs end) must not be able to
    // leave a truncated registry behind.
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        lucent::warn("gfxdb", "could not replace {} with the new registry.", path);
    }
}

void sbr_gfxdb_report() {
    // The control runs FIRST and in BOTH branches, because a control that only runs on the happy
    // path is not a control. SBR_GFXDB=0 is its negative case, and it is meant to be run: with
    // detection off, every draw an emitter site would have claimed must show up here instead. If
    // this number is small in that run, the label is not what is attributing the draws and every
    // per-emitter measurement in the registry is suspect.
    long unlabelled[kDispCount];
    aurora::gfx::interp::audit_row(SB_POP_UNLABELLED, unlabelled, kDispCount);
    long unlabelledDraws = 0;
    for (int d = 0; d < kDispCount; ++d) unlabelledDraws += unlabelled[d];

    if (!sbr_gfxdb_enabled()) {
        lucent::info("gfxdb", "graphics registry DISABLED (SBR_GFXDB=0): nothing was detected and "
                              "nothing written. This is not a statement that the run drew nothing — "
                              "and as the control for the enabled case, {} draw(s) are filed under "
                              "(unlabelled) here (a count the interpolation audit only produces "
                              "with SBR_LERP60=1, so run the control with both set). With detection ON that number must fall to ~0; if "
                              "it does not, the labels are not doing the attributing.",
                     unlabelledDraws);
        return;
    }
    if (unlabelledDraws != 0) {
        lucent::warn("gfxdb", "{} draw(s) were still filed under (unlabelled) — no seam and no "
                              "detected site claimed them. Detection has a hole: geometry is "
                              "reaching the fifo through a waist this build does not hook.",
                     unlabelledDraws);
    }
    if (aurora::gfx::interp::max_populations() < kPopCount) {
        lucent::error("gfxdb", "aurora tracks only {} population(s) but this build allocates ids up "
                               "to {}. Every id past aurora's ceiling is folded into (unlabelled) "
                               "there, so the measurements for those sites are WRONG, not missing.",
                      aurora::gfx::interp::max_populations(), kPopCount);
    }
    unsigned long sites = 0, calls = 0;
    for (int id = kFirstSiteId; id < kPopCount; ++id) {
        if (g_sites[id].used) {
            ++sites;
            calls += g_sites[id].calls;
        }
    }
    lucent::info("gfxdb",
                 "graphics registry: {} emitter site(s) detected this run ({} new to this process), "
                 "{} waist call(s) attributed, {} site(s) lost to id-space overflow ({} call(s)). "
                 "{}  File: {}",
                 sites, g_newThisRun, calls, g_overflowSites.size(), g_overflowCalls,
                 sites == 0 ? "NO SITE WAS DETECTED AT ALL — either nothing was drawn through the "
                              "hooked waists, or the hooks are not running. Both look like this "
                              "line, so check the run rendered before reading it as coverage."
                            : "Every site with no curated verdict is a graphic nobody has looked at.",
                 db_path().empty() ? "<not persisted: no repo root>" : db_path());
}
