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

// Resolve a possibly-namespaced type name to its parsed definition via the header index (same
// name-matching the layout composer uses: exact, then namespace-stripped, then bare leaf).
ParsedType resolve_parse(const std::string& type_name,
                         const std::map<std::string, std::string>& index) {
    auto it = index.find(type_name);
    if (it == index.end()) it = index.find(strip_ns(type_name));
    if (it == index.end()) it = index.find(base_name(type_name));
    if (it == index.end()) return ParsedType{};
    // For a nested type ("Outer::Inner") the index maps the LEAF to its header; re-parse with the
    // qualified name so the nested-body narrowing in parse_decomp_type kicks in.
    if (type_name.find("::") != std::string::npos)
        return parse_decomp_file(it->second, type_name);
    return parse_decomp_file(it->second, it->first);
}

// One field's guest size/alignment; false if it is an embedded value type we can't size.
bool field_guest_size(const ParsedField& f, int& sz, int& al) {
    if (f.is_pointer) { sz = 4 * (f.array_count > 0 ? f.array_count : 1); al = 4; return true; }
    if (f.sizable) {
        LField lf{ f.name, f.kind, f.pointee, f.array_count };
        field_size_align(lf, guest_abi(), sz, al);
        return true;
    }
    return false;   // embedded value type / enum / unknown — not sized here
}

int round_up_to(int v, int a) { return a <= 1 ? v : ((v + a - 1) / a) * a; }

// Guest object size of `type_name`: own annotated fields + single-inheritance base chain + the
// CodeWarrior vtable slot (at 0 for a root polymorphic class; APPENDED at the base size for a
// class introducing virtuals over a NON-polymorphic base — abi_findings.md). Returns 0 when
// indeterminable (an embedded value field could extend the type) — recognition then skips the
// size signal and keeps the signature type (the honest fallback; a wrong size can only MISS an
// upgrade, never force a false one, because the guest `li` is the ground-truth comparand).
int guest_size_impl(const std::string& type_name,
                    const std::map<std::string, std::string>& index, std::set<std::string>& seen) {
    if (!seen.insert(type_name).second) return 0;                 // cycle guard
    ParsedType t = resolve_parse(type_name, index);
    if (!t.found) { seen.erase(type_name); return 0; }
    bool has_base = !t.base.empty();
    int base_sz = 0; bool base_poly = false;
    if (has_base) {
        base_sz = guest_size_impl(t.base, index, seen);
        if (base_sz == 0) { seen.erase(type_name); return 0; }    // base unsized -> whole unsized
        base_poly = resolve_parse(t.base, index).polymorphic;
    }
    seen.erase(type_name);
    int max_end = base_sz, align = 4, max_unknown_off = -1;
    for (const auto& f : t.fields) {
        int fsz, fal;
        if (field_guest_size(f, fsz, fal)) {
            if (f.offset + fsz > max_end) max_end = f.offset + fsz;
            if (fal > align) align = fal;
        } else if (f.offset > max_unknown_off) max_unknown_off = f.offset;
    }
    if (max_unknown_off >= max_end) return 0;                     // an unsized field could extend it
    int sz = max_end;
    if (t.polymorphic && has_base && !base_poly) sz += 4;         // vtable appended at base size
    else if (t.polymorphic && !has_base && sz < 4) sz = 4;        // vtable@0 root, no data members
    return round_up_to(sz, align);
}

// The appended-vtable guest offset for a class that introduces virtuals over a NON-polymorphic
// base (== base size), or -1 if this type has no appended vtable. Used to MAP that vtable store
// in the layout so a polymorphic subclass construction is not a recovery gap (the store is then a
// recognized — bridge-routed — construction detail, not unmapped memory).
int appended_vtable_offset(const std::string& type_name,
                           const std::map<std::string, std::string>& index) {
    ParsedType t = resolve_parse(type_name, index);
    if (!t.found || !t.polymorphic || t.base.empty()) return -1;
    if (resolve_parse(t.base, index).polymorphic) return -1;      // reuses the base's vtable slot
    std::set<std::string> seen;
    return guest_size_impl(t.base, index, seen);                  // = base size
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
            // Carry the sub-field's POINTER KIND (nested_type / guest_ptr) forward — an embedded
            // value type's pointer member is still a pointer (engine handle or guest-data xlate),
            // never a plain scalar. Dropping guest_ptr here truncated a host pointer to u32 (LP64
            // precision loss) in the bridged getter; the embedded mVertexData/mDrawMtxData arrays
            // are exactly this case.
            L.fields[f.offset + off] = FieldDesc{ f.name + "." + fd.member, fd.nested_type, fd.guest_ptr };
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
    EngineLayout L = build_layout(type_name, index, engine_types, seen, unresolved);
    // Map the appended vtable slot of a polymorphic-over-non-polymorphic-base class so its
    // construction's vtable store is recognized (bridge-routed), not an unmapped-offset gap.
    int vt = appended_vtable_offset(type_name, index);
    if (vt >= 0 && !L.fields.count(vt)) L.fields[vt] = FieldDesc{ "__vtbl", "" };
    return L;
}

namespace {

// One discovered class/struct definition: fully-qualified name (nested types carry their
// enclosing scope, e.g. "J2DWindow::Texture"), its first base (as written), and the header.
struct TypeDef { std::string name, base, header; };

// Capture the first real base name out of a "... : public A, B" base list starting after ':'.
std::string capture_base(const std::string& text, size_t colon, size_t stop) {
    std::string base, tok;
    for (size_t j = colon + 1; j < stop && text[j] != ',' && text[j] != '{'; ++j) {
        char b = text[j];
        if (is_ident(b) || b == ':') tok += b;
        else if (!tok.empty()) {
            if (tok != "public" && tok != "protected" && tok != "private" && tok != "virtual")
                { base = tok; break; }
            tok.clear();
        }
    }
    if (base.empty() && !tok.empty() && tok != "public" && tok != "protected" &&
        tok != "private" && tok != "virtual") base = tok;
    return base;
}

// Scan a header for every class/struct DEFINITION, tracking an enclosing-scope stack so nested
// types get a qualified name. Focused (no template-parameter handling); honest about its limits.
void scan_header_types(const std::string& text, const std::string& path, std::vector<TypeDef>& out) {
    std::vector<std::pair<std::string, int>> scope;     // (name, brace depth inside the body)
    int depth = 0;
    for (size_t i = 0; i < text.size();) {
        bool kw = false; size_t klen = 0;
        if (text.compare(i, 6, "class ") == 0) { kw = true; klen = 6; }
        else if (text.compare(i, 7, "struct ") == 0) { kw = true; klen = 7; }
        if (kw && (i == 0 || !is_ident(text[i - 1]))) {
            size_t ns = i + klen;
            while (ns < text.size() && (text[ns] == ' ' || text[ns] == '\t')) ++ns;
            size_t ne = ns; while (ne < text.size() && is_ident(text[ne])) ++ne;
            std::string name = text.substr(ns, ne - ns);
            size_t j = ne; std::string base; bool defn = false, fwd = false;
            for (; j < text.size(); ++j) {
                if (text[j] == ';') { fwd = true; break; }
                if (text[j] == '{') { defn = true; break; }
                if (text[j] == ':' && (j + 1 >= text.size() || text[j + 1] != ':'))
                    base = capture_base(text, j, text.size());
            }
            if (!name.empty() && defn) {
                std::string qual;
                for (auto& s : scope) { qual += s.first; qual += "::"; }
                qual += name;
                out.push_back({ qual, base, path });
                ++depth;                          // consume the body-opening '{'
                scope.push_back({ name, depth });
                i = j + 1; continue;
            }
            if (fwd) { i = j + 1; continue; }
            i = ne; continue;
        }
        char ch = text[i];
        if (ch == '{') ++depth;
        else if (ch == '}') { --depth; while (!scope.empty() && scope.back().second > depth) scope.pop_back(); }
        ++i;
    }
}

// All class/struct definitions across `include_dir`, with qualified names + first base.
std::vector<TypeDef> all_type_defs(const std::string& include_dir) {
    std::vector<std::string> headers; walk(include_dir, headers);
    std::vector<TypeDef> defs;
    for (const auto& h : headers) scan_header_types(slurp(h), h, defs);
    return defs;
}

// Subclasses (transitive, single-inheritance) of any type in `roots`, by matching each def's base
// to a root/already-found type by LEAF name (bases are written unqualified or namespace-qualified;
// a leaf match is sufficient for the unique engine leaves we flip and is HONEST about ambiguity).
std::set<std::string> discover_subclasses(const std::set<std::string>& roots,
                                          const std::vector<TypeDef>& defs) {
    auto leaf = [](const std::string& s) {
        size_t lt = s.find('<'); std::string h = (lt == std::string::npos) ? s : s.substr(0, lt);
        size_t ns = h.rfind("::"); return ns == std::string::npos ? h : h.substr(ns + 2);
    };
    std::set<std::string> known_leaves;
    for (const auto& r : roots) known_leaves.insert(leaf(r));
    std::set<std::string> found;
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& d : defs) {
            if (d.base.empty() || found.count(d.name) || roots.count(d.name)) continue;
            if (known_leaves.count(leaf(d.base))) {
                found.insert(d.name);
                known_leaves.insert(leaf(d.name));
                grew = true;
            }
        }
    }
    return found;
}

}  // namespace

std::map<u32, std::string> build_return_types(const std::set<std::string>& active_types,
                                              const std::map<std::string, std::string>& index,
                                              const std::string& funcs_txt_path) {
    std::map<u32, std::string> out;
    std::ifstream in(funcs_txt_path);
    if (!in) return out;
    // The return type as written -> active engine leaf if it is a pointer/ref to one, else "".
    auto engine_leaf = [&](const std::string& ret) -> std::string {
        if (ret.find('*') == std::string::npos && ret.find('&') == std::string::npos) return "";
        std::string r = ret;
        for (char& c : r) if (c == '*' || c == '&') c = ' ';   // drop pointer/ref markers
        // strip const/volatile tokens
        std::string cleaned;
        std::istringstream ts(r); std::string tk;
        while (ts >> tk) { if (tk != "const" && tk != "volatile") { if (!cleaned.empty()) cleaned += ' '; cleaned += tk; } }
        std::string leaf = base_name(cleaned);
        return active_types.count(leaf) ? leaf : "";
    };
    std::map<std::string, ParsedType> cache;                   // class leaf -> parsed (memoize)
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string addr_s, name;
        if (!(ls >> addr_s >> name)) continue;
        u32 addr; try { addr = (u32)std::stoul(addr_s, nullptr, 16); } catch (...) { continue; }
        FuncSig sig = demangle_signature(name);
        if (!sig.ok || !sig.is_method || sig.method_name.empty()) continue;
        auto it = cache.find(sig.class_leaf);
        if (it == cache.end()) it = cache.emplace(sig.class_leaf, resolve_parse(sig.class_name, index)).first;
        const ParsedType& pt = it->second;
        if (!pt.found) continue;
        auto mr = pt.method_returns.find(sig.method_name);
        if (mr == pt.method_returns.end()) continue;
        std::string leaf = engine_leaf(mr->second);
        if (!leaf.empty()) out[addr] = leaf;
    }
    return out;
}

TypeDBBuildResult build_type_db(const std::set<std::string>& active_types,
                                const std::string& include_dir,
                                const std::string& funcs_txt_path) {
    TypeDBBuildResult r;
    std::map<std::string, std::string> index = index_headers(include_dir);
    r.header_count = (int)index.size();

    // Discover polymorphic/derived subclasses of the active types so a `new Subclass` is recognized
    // as the subclass (its appended-vtable construction routes to the ctor bridge), not the base.
    std::vector<TypeDef> defs = all_type_defs(include_dir);
    std::map<std::string, std::string> base_of;
    for (const auto& d : defs)                              // qualified-name -> header / first base
        if (!d.name.empty()) {
            if (d.name.find("::") != std::string::npos && !index.count(d.name)) index[d.name] = d.header;
            if (!base_of.count(d.name)) base_of[d.name] = d.base;
        }
    std::set<std::string> subs = discover_subclasses(active_types, defs);
    std::set<std::string> all = active_types;
    for (const auto& s : subs) all.insert(s);

    auto leaf = [](const std::string& s) {
        size_t lt = s.find('<'); std::string h = (lt == std::string::npos) ? s : s.substr(0, lt);
        size_t ns = h.rfind("::"); return ns == std::string::npos ? h : h.substr(ns + 2);
    };

    for (const auto& ty : all) {
        ParsedType pt = resolve_parse(ty, index);
        if (!pt.found) { if (active_types.count(ty)) r.missing_types.push_back(ty); continue; }
        if (index.count(ty)) r.type_headers[ty] = index[ty];   // for the accessor-TU #includes
        r.db.layouts[ty] = compose_layout(ty, index, all, &r.unresolved_bases);
        std::set<std::string> seen;
        int sz = guest_size_impl(ty, index, seen);
        if (sz > 0) r.db.sizes[ty] = sz;
        // Record the immediate base mapped to a DB key (by leaf) so subclass-depth walks resolve.
        std::string cb = base_of.count(ty) ? base_of[ty] : pt.base;
        if (!cb.empty()) {
            std::string key;
            for (const auto& u : all) if (leaf(u) == leaf(cb)) { key = u; if (u == cb) break; }
            r.db.bases[ty] = key.empty() ? cb : key;
        }
    }
    r.db.signatures = build_signatures(funcs_txt_path, active_types);
    r.db.return_types = build_return_types(all, index, funcs_txt_path);
    return r;
}
