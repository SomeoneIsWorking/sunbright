#include "type_db_build.h"
#include "decomp_parse.h"
#include "func_sig.h"

#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace {

void walk(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string p = dir + "/" + n;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk(p, out);
        else if ((n.size() >= 4 && n.substr(n.size()-4) == ".hpp") ||
                 (n.size() >= 2 && n.substr(n.size()-2) == ".h"))
            out.push_back(p);
    }
    closedir(d);
}

std::string slurp(const std::string& p) {
    std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

bool is_ident(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Strip only a leading namespace ("a::b::X<f>" -> "X<f>"), keeping any template arg list.
std::string strip_ns(const std::string& t) {
    size_t lt = t.find('<');
    std::string head = (lt == std::string::npos) ? t : t.substr(0, lt);
    size_t ns = head.rfind("::");
    return ns == std::string::npos ? t : t.substr(ns + 2);
}

// Strip a leading namespace AND a trailing template arg list ("a::b::X<f>" -> "X").
std::string base_name(const std::string& t) {
    std::string s = strip_ns(t);
    size_t lt = s.find('<');
    return lt == std::string::npos ? s : s.substr(0, lt);
}

// Sub-field of an embedded VALUE type: host member + offset within the value.
struct Sub { std::string member; int off; };

// Known templated / builtin value types the focused parser can't crack (templates, dolphin
// typedefs). Member names mirror the decomp/JGeometry/GX field names the port/ engine uses.
const std::map<std::string, std::vector<Sub>>& known_embedded() {
    static const std::map<std::string, std::vector<Sub>> tab = {
        { "Vec",        { {"x",0}, {"y",4}, {"z",8} } },                 // f32 x3
        { "Point3d",    { {"x",0}, {"y",4}, {"z",8} } },
        { "TVec3<f32>", { {"x",0}, {"y",4}, {"z",8} } },                 // JGeometry::TVec3<f32> : Vec
        { "TVec2<f32>", { {"x",0}, {"y",4} } },
        { "S16Vec",     { {"x",0}, {"y",2}, {"z",4} } },                 // s16 x3
        { "TVec3<s16>", { {"x",0}, {"y",2}, {"z",4} } },
        { "TVec2<s16>", { {"x",0}, {"y",2} } },
        { "GXColor",    { {"r",0}, {"g",1}, {"b",2}, {"a",3} } },        // u8 RGBA
        { "TColor",     { {"r",0}, {"g",1}, {"b",2}, {"a",3} } },
        { "GXColorS10", { {"r",0}, {"g",2}, {"b",4}, {"a",6} } },        // s16 RGBA
    };
    return tab;
}

}  // namespace

// forward decls (mutually recursive: base chain <-> embedded expansion)
static EngineLayout build_layout(const std::string& type_name,
                                 const std::map<std::string, std::string>& index,
                                 const std::set<std::string>& engine_types,
                                 std::set<std::string>& seen, std::vector<std::string>* unresolved);

// The expanded sub-layout (offsets relative to the value) of an embedded VALUE-type field, or
// empty if we can't determine it (then the caller keeps a single best-effort entry and any
// sub-offset access shows up as a coverage gap — honest, not a silent wrong field).
static EngineLayout embedded_layout(const std::string& type_str,
                                    const std::map<std::string, std::string>& index,
                                    const std::set<std::string>& engine_types,
                                    std::set<std::string>& seen, std::vector<std::string>* unresolved) {
    EngineLayout L;
    // 1. known templated/builtin value types
    auto try_tab = [&](const std::string& key) -> bool {
        auto it = known_embedded().find(key);
        if (it == known_embedded().end()) return false;
        for (const auto& s : it->second) L.fields[s.off] = FieldDesc{ s.member, "" };
        return true;
    };
    if (try_tab(type_str) || try_tab(strip_ns(type_str)) || try_tab(base_name(type_str))) return L;
    // 2. a concrete (non-template) struct in the headers -> recurse
    std::string leaf = base_name(type_str);
    if (type_str.find('<') == std::string::npos && index.count(leaf))
        return build_layout(leaf, index, engine_types, seen, unresolved);
    return L;   // unknown value type
}

// Add one parsed field to a layout, expanding embedded value types into their sub-fields.
static void add_field(EngineLayout& L, const ParsedField& f,
                      const std::map<std::string, std::string>& index,
                      const std::set<std::string>& engine_types,
                      std::set<std::string>& seen, std::vector<std::string>* unresolved) {
    if (f.is_pointer) {
        const bool engine = engine_types.count(f.pointee) != 0;
        // engine-object pointer -> nested_type (handle); pointer to GUEST data -> guest_ptr
        // (host<->guest translation). A pointer is never a plain scalar — one of the two.
        L.fields[f.offset] = FieldDesc{ f.name, engine ? f.pointee : "", /*guest_ptr=*/!engine };
        return;
    }
    if (f.sizable) {                               // scalar (incl. scalar arrays)
        L.fields[f.offset] = FieldDesc{ f.name, "" };
        return;
    }
    // embedded value type / enum / unknown — try to expand its sub-fields
    EngineLayout sub = embedded_layout(f.type, index, engine_types, seen, unresolved);
    if (!sub.fields.empty())
        for (const auto& [off, fd] : sub.fields)
            L.fields[f.offset + off] = FieldDesc{ f.name + "." + fd.member, fd.nested_type };
    else
        L.fields[f.offset] = FieldDesc{ f.name, "" };   // best effort; sub-offsets stay gaps
}

// Fully expanded layout for `type_name`: base chain (single inheritance, base subobject at 0,
// so base fields are already object-absolute) + own fields, embedded value types expanded.
static EngineLayout build_layout(const std::string& type_name,
                                 const std::map<std::string, std::string>& index,
                                 const std::set<std::string>& engine_types,
                                 std::set<std::string>& seen, std::vector<std::string>* unresolved) {
    EngineLayout L;
    if (seen.count(type_name)) return L;                 // cycle guard
    // Resolve against the bare-name index (a base may be written "JDrama::TNameRef").
    auto it = index.find(type_name);
    if (it == index.end()) it = index.find(strip_ns(type_name));
    if (it == index.end()) it = index.find(base_name(type_name));
    if (it == index.end()) { if (unresolved) unresolved->push_back(type_name); return L; }
    // Parse with the matched BARE key (the header declares "class TNameRef", not "JDrama::TNameRef").
    ParsedType t = parse_decomp_file(it->second, it->first);
    if (!t.found) { if (unresolved) unresolved->push_back(type_name); return L; }
    seen.insert(type_name);
    if (!t.base.empty()) L = build_layout(t.base, index, engine_types, seen, unresolved);
    for (const auto& f : t.fields) add_field(L, f, index, engine_types, seen, unresolved);
    seen.erase(type_name);
    return L;
}

std::map<std::string, std::string> index_headers(const std::string& include_dir) {
    std::map<std::string, std::string> index;
    std::vector<std::string> headers;
    walk(include_dir, headers);
    for (const auto& h : headers) {
        std::string text = slurp(h);
        for (const char* kw : {"class ", "struct "}) {
            size_t p = 0;
            while ((p = text.find(kw, p)) != std::string::npos) {
                size_t ns = p + std::strlen(kw);
                size_t ne = ns;
                while (ne < text.size() && is_ident(text[ne])) ++ne;
                std::string name = text.substr(ns, ne - ns);
                p = ne;
                if (name.empty() || index.count(name)) continue;
                // Record only a real DEFINITION (parse finds a body with fields or a base).
                ParsedType t = parse_decomp_type(text, name);
                if (t.found && (!t.fields.empty() || !t.base.empty()))
                    index[name] = h;
            }
        }
    }
    return index;
}

EngineLayout compose_layout(const std::string& type_name,
                            const std::map<std::string, std::string>& index,
                            const std::set<std::string>& engine_types,
                            std::vector<std::string>* unresolved) {
    std::set<std::string> seen;
    return build_layout(type_name, index, engine_types, seen, unresolved);
}

TypeDBBuildResult build_type_db(const std::set<std::string>& active_types,
                                const std::string& include_dir,
                                const std::string& funcs_txt_path) {
    TypeDBBuildResult r;
    std::map<std::string, std::string> index = index_headers(include_dir);
    r.header_count = (int)index.size();

    for (const auto& ty : active_types) {
        if (!index.count(ty)) { r.missing_types.push_back(ty); continue; }
        r.db.layouts[ty] = compose_layout(ty, index, active_types, &r.unresolved_bases);
    }
    r.db.signatures = build_signatures(funcs_txt_path, active_types);
    return r;
}
