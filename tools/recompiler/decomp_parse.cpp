#include "decomp_parse.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

// See decomp_parse.h. Focused line-oriented parser for the regular member shapes the SMS
// decomp uses. Everything here is pure text processing (portable C++17).

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Read a leading hex run (optional 0x prefix), stopping at the first non-hex char.
// Succeeds if at least one digit was read — for "// _02, comment" tails.
bool leading_hex(const std::string& s, int& out) {
    size_t i = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
    long v = 0; bool any = false;
    for (; i < s.size(); ++i) {
        char c = s[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + d; any = true;
    }
    if (!any) return false;
    out = (int)v;
    return true;
}

// Parse "0x1A" / "1A" (hex) or a leading offset annotation's hex digits.
bool parse_hex(const std::string& s, int& out) {
    if (s.empty()) return false;
    size_t i = 0;
    std::string h = s;
    if (h.size() > 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) i = 2;
    long v = 0; bool any = false;
    for (; i < h.size(); ++i) {
        char c = h[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + d; any = true;
    }
    if (!any) return false;
    out = (int)v;
    return true;
}

// The annotated guest offset on a member line, or nullopt. Recognises both decomp styles:
//   /* 0x18 */ ...      (leading block comment)
//   ... ; // _18        (trailing line comment)
std::optional<int> extract_offset(const std::string& line) {
    // Leading block style: /* 0x<hex> */
    size_t p = line.find("/*");
    while (p != std::string::npos) {
        size_t q = line.find("*/", p + 2);
        if (q == std::string::npos) break;
        std::string inner = trim(line.substr(p + 2, q - (p + 2)));
        if (inner.size() > 2 && (inner[0] == '0') && (inner[1] == 'x' || inner[1] == 'X')) {
            int v; if (parse_hex(inner, v)) return v;
        }
        p = line.find("/*", q + 2);
    }
    // Trailing line style: // _<hex>
    size_t c = line.find("//");
    if (c != std::string::npos) {
        std::string inner = trim(line.substr(c + 2));
        if (!inner.empty() && inner[0] == '_') {
            int v; if (leading_hex(inner.substr(1), v)) return v;  // "_02, comment" -> 0x02
        }
    }
    return std::nullopt;
}

// Remove /* */ and // comments from a line.
std::string strip_comments(const std::string& line) {
    std::string out;
    for (size_t i = 0; i < line.size();) {
        if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '*') {
            size_t q = line.find("*/", i + 2);
            if (q == std::string::npos) break;
            out += ' ';
            i = q + 2;
        } else if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            break;
        } else {
            out += line[i++];
        }
    }
    return out;
}

bool is_ident_char(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Remove every __attribute__((...)) span (balanced parens) from a declaration. The decomp
// uses these for explicit alignment (e.g. `u8 x __attribute__((aligned(4)))`); we drop them
// for parsing (any layout shift they'd cause is caught by the offset-annotation validation).
std::string strip_attributes(const std::string& s) {
    std::string out = s;
    size_t a;
    while ((a = out.find("__attribute__")) != std::string::npos) {
        size_t p = a + 13;
        while (p < out.size() && (out[p] == ' ' || out[p] == '\t')) ++p;
        if (p >= out.size() || out[p] != '(') { out.erase(a, 13); continue; }
        int depth = 0; size_t q = p;
        for (; q < out.size(); ++q) {
            if (out[q] == '(') ++depth;
            else if (out[q] == ')') { if (--depth == 0) { ++q; break; } }
        }
        out.erase(a, q - a);
    }
    return out;
}

// Evaluate a simple integer dimension expression: terms (hex/dec) joined by + / -.
bool eval_dim(const std::string& expr, int& out) {
    std::string e = trim(expr);
    if (e.empty()) return false;
    long acc = 0; int sign = 1; size_t i = 0; bool any = false;
    while (i < e.size()) {
        while (i < e.size() && (e[i] == ' ' || e[i] == '\t')) ++i;
        if (i < e.size() && (e[i] == '+' || e[i] == '-')) { sign = (e[i] == '-') ? -1 : 1; ++i; continue; }
        size_t j = i;
        while (j < e.size() && (is_ident_char(e[j]))) ++j;
        if (j == i) return false;
        std::string tok = e.substr(i, j - i);
        long v;
        int hv;
        if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
            if (!parse_hex(tok, hv)) return false;
            v = hv;
        } else {
            try { v = std::stol(tok); } catch (...) { return false; }
        }
        acc += sign * v; sign = 1; any = true; i = j;
    }
    if (!any) return false;
    out = (int)acc;
    return true;
}

// Map a scalar/typedef type name to FKind. Returns false for non-scalar types.
bool scalar_kind(const std::string& ty, FKind& k) {
    static const std::vector<std::pair<std::string, FKind>> tab = {
        {"s8", FKind::I8}, {"char", FKind::I8}, {"signed char", FKind::I8},
        {"u8", FKind::U8}, {"bool", FKind::U8}, {"unsigned char", FKind::U8},
        {"s16", FKind::I16}, {"short", FKind::I16}, {"signed short", FKind::I16},
        {"u16", FKind::U16}, {"unsigned short", FKind::U16}, {"wchar_t", FKind::U16},
        {"s32", FKind::I32}, {"int", FKind::I32}, {"long", FKind::I32}, {"signed", FKind::I32},
        {"signed int", FKind::I32}, {"signed long", FKind::I32}, {"BOOL", FKind::I32},
        {"u32", FKind::U32}, {"unsigned", FKind::U32}, {"unsigned int", FKind::U32},
        {"unsigned long", FKind::U32},
        {"s64", FKind::I64}, {"long long", FKind::I64}, {"signed long long", FKind::I64},
        {"u64", FKind::U64}, {"unsigned long long", FKind::U64},
        {"f32", FKind::F32}, {"float", FKind::F32},
        {"f64", FKind::F64}, {"double", FKind::F64},
    };
    for (const auto& e : tab) if (e.first == ty) { k = e.second; return true; }
    return false;
}

// Strip leading qualifiers and squeeze internal whitespace.
std::string normalize_type(std::string t) {
    t = trim(t);
    // strip leading const/volatile/mutable/struct/class/enum keyword noise
    static const char* drop[] = {"const ", "volatile ", "mutable ", "struct ", "class ", "enum "};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char* d : drop) {
            size_t n = std::string(d).size();
            if (t.compare(0, n, d) == 0) { t = trim(t.substr(n)); changed = true; }
        }
    }
    // squeeze runs of spaces
    std::string out; bool sp = false;
    for (char c : t) {
        if (c == ' ' || c == '\t') { if (!sp && !out.empty()) out += ' '; sp = true; }
        else { out += c; sp = false; }
    }
    return trim(out);
}

// The method name declared by a `virtual ...` line (comments already stripped), or "".
// Returns the identifier (or "~Name" destructor) immediately before the first '(':
//   "virtual void calc()"        -> "calc"
//   "virtual ~J3DModel()"        -> "~J3DModel"
//   "virtual void calc(J3DModel*) = 0" -> "calc"
std::string virtual_method_name(const std::string& code) {
    size_t lp = code.find('(');
    if (lp == std::string::npos) return "";
    // walk back over whitespace, then collect an identifier run (and a leading '~').
    size_t e = lp;
    while (e > 0 && (code[e - 1] == ' ' || code[e - 1] == '\t')) --e;
    size_t s = e;
    while (s > 0 && is_ident_char(code[s - 1])) --s;
    if (s == e) return "";
    if (s > 0 && code[s - 1] == '~') --s;          // destructor
    std::string name = code.substr(s, e - s);
    // reject operator-call false positives / control keywords
    if (name == "if" || name == "for" || name == "while" || name == "switch" ||
        name == "return" || name == "sizeof") return "";
    return name;
}

}  // namespace

// Defined below; parses one member declaration line into `f`.
static bool parse_field_decl(const std::string& decl_in, ParsedField& f);

// Locate a class/struct DEFINITION named `type_name` in `text`: its base (first base), and the
// [body_start, body_end) span between the matching braces. Skips forward declarations. Returns
// found=false if no definition is present.
struct ClassLoc { bool found = false; std::string base; size_t body_start = std::string::npos, body_end = std::string::npos; };
static ClassLoc locate_class(const std::string& text, const std::string& type_name) {
    ClassLoc loc;
    size_t pos = 0;
    while (true) {
        size_t c = text.find("class " + type_name, pos);
        size_t s = text.find("struct " + type_name, pos);
        size_t at = std::min(c, s);
        if (at == std::string::npos) break;
        size_t after = at + (at == c ? 6 : 7) + type_name.size();
        pos = after;
        if (after < text.size() && is_ident_char(text[after])) continue;   // word boundary
        size_t i = after;
        std::string base;
        bool defn = false;
        while (i < text.size()) {
            char ch = text[i];
            if (ch == ';') break;                 // forward declaration -> keep searching
            if (ch == '{') { defn = true; break; }
            if (ch == ':') {
                size_t j = i + 1;
                std::string tok;
                while (j < text.size() && text[j] != '{' && text[j] != ',') {
                    char b = text[j];
                    if (is_ident_char(b) || b == ':') tok += b;
                    else if (!tok.empty()) {
                        if (tok != "public" && tok != "protected" && tok != "private" &&
                            tok != "virtual") { if (base.empty()) base = tok; }
                        tok.clear();
                    }
                    ++j;
                }
                if (!tok.empty() && base.empty() && tok != "public" && tok != "protected" &&
                    tok != "private" && tok != "virtual") base = tok;
                i = j;
                continue;
            }
            ++i;
        }
        if (!defn) continue;
        loc.base = base;
        loc.body_start = i + 1;
        int depth = 1;
        size_t k = loc.body_start;
        for (; k < text.size(); ++k) {
            if (text[k] == '{') ++depth;
            else if (text[k] == '}') { if (--depth == 0) { loc.body_end = k; break; } }
        }
        loc.found = (loc.body_end != std::string::npos);
        break;
    }
    return loc;
}

ParsedType parse_decomp_type(const std::string& text, const std::string& type_name) {
    // Qualified nested type "Outer::Inner[::...]": narrow to the outer class's body, then parse the
    // remaining (possibly still-qualified) name inside it. This is how a nested engine subclass like
    // J2DWindow::Texture is reached (object_identity.md — most-derived recognition).
    size_t sep = type_name.find("::");
    if (sep != std::string::npos) {
        std::string outer = type_name.substr(0, sep), rest = type_name.substr(sep + 2);
        ClassLoc oc = locate_class(text, outer);
        if (!oc.found) { ParsedType pt; pt.name = type_name; pt.found = false; return pt; }
        ParsedType inner = parse_decomp_type(text.substr(oc.body_start, oc.body_end - oc.body_start), rest);
        inner.name = type_name;       // keep the fully-qualified name
        return inner;
    }

    ParsedType pt;
    pt.name = type_name;

    // Locate the definition (skips forward declarations), capturing the first base and body span.
    ClassLoc loc = locate_class(text, type_name);
    if (!loc.found) { pt.found = false; return pt; }
    pt.base = loc.base;
    pt.found = true;
    size_t body_start = loc.body_start, body_end = loc.body_end;

    std::string body = text.substr(body_start, body_end - body_start);

    // Walk the body line by line, tracking brace depth so nested blocks (method bodies,
    // nested struct/union/enum) at depth>0 are skipped. Members are at depth 0.
    std::istringstream iss(body);
    std::string raw;
    int depth = 0;
    while (std::getline(iss, raw)) {
        if (raw.find("virtual") != std::string::npos) pt.polymorphic = true;

        std::optional<int> ann = (depth == 0) ? extract_offset(raw) : std::nullopt;
        std::string code = strip_comments(raw);

        // Capture a member-level `virtual` method declaration in declaration order (the vtable
        // slot order). Only at depth 0 (member scope), and only the first such name per line.
        if (depth == 0) {
            size_t v = code.find("virtual");
            bool tok = v != std::string::npos &&
                       (v == 0 || !is_ident_char(code[v - 1])) &&
                       (v + 7 >= code.size() || !is_ident_char(code[v + 7]));
            if (tok) {
                std::string name = virtual_method_name(code.substr(v + 7));
                if (!name.empty()) pt.virtuals.push_back(name);
            }
        }

        // count brace delta for this line (so we can decide membership *before* descending)
        int open = 0, close = 0;
        for (char ch : code) { if (ch == '{') ++open; else if (ch == '}') ++close; }

        if (depth == 0 && ann) {
            ParsedField f;
            f.offset = *ann;
            // take the declaration up to the first ';'
            size_t sc = code.find(';');
            std::string decl = trim(code.substr(0, sc == std::string::npos ? code.size() : sc));
            if (parse_field_decl(decl, f)) pt.fields.push_back(f);
        }
        depth += open - close;
        if (depth < 0) depth = 0;
    }

    // fully_sizable: no base subobject and every field has a sound size model.
    pt.fully_sizable = pt.base.empty();
    for (const auto& f : pt.fields) if (!f.sizable) pt.fully_sizable = false;
    return pt;
}

// Parse one member declaration (comments/`;` already removed), e.g.:
//   "s16 mXAngleMin"        "J2DPane* unk20[5]"     "void (*mCallback)(u32)"
// Returns false if the line isn't a data member (method, static, typedef, ...).
static bool parse_field_decl(const std::string& decl_in, ParsedField& f) {
    std::string decl = trim(strip_attributes(decl_in));
    if (decl.empty()) return false;

    // reject non-member declarations
    static const char* reject[] = {"static ", "typedef ", "using ", "friend ", "virtual ",
                                   "static\t", "typedef\t"};
    for (const char* r : reject) if (decl.compare(0, std::string(r).size(), r) == 0) return false;
    if (decl == "public" || decl == "private" || decl == "protected") return false;

    // Function pointer: "<ret> (*name)(args)"  -> a pointer member.
    size_t fp = decl.find("(*");
    if (fp != std::string::npos) {
        size_t ns = fp + 2;
        size_t ne = ns;
        while (ne < decl.size() && is_ident_char(decl[ne])) ++ne;
        f.name = decl.substr(ns, ne - ns);
        f.type = normalize_type(decl.substr(0, fp)) + " (*)()";
        f.is_pointer = true;
        f.pointee = "";
        f.kind = FKind::Ptr;
        f.sizable = !f.name.empty();
        return f.sizable;
    }

    // Plain method (has parentheses but isn't a function pointer) -> skip.
    if (decl.find('(') != std::string::npos) return false;

    // Array dimensions: collect and strip them, leaving "<type+stars> <name>".
    int count = 1;
    std::string head = decl;
    {
        size_t b = head.find('[');
        if (b != std::string::npos) {
            std::string dims = head.substr(b);
            head = trim(head.substr(0, b));
            // multiply all [..] dimensions
            size_t i = 0;
            while (i < dims.size()) {
                if (dims[i] == '[') {
                    size_t close = dims.find(']', i);
                    if (close == std::string::npos) return false;
                    int d;
                    if (!eval_dim(dims.substr(i + 1, close - i - 1), d) || d <= 0) {
                        f.array_count = 1; f.sizable = false; count = 1;
                        // unknown dimension: keep going but mark unsizable below
                        d = 1;
                    }
                    count *= d;
                    i = close + 1;
                } else ++i;
            }
        }
    }

    // Split head into the trailing identifier (name) and the type (everything before).
    size_t e = head.size();
    while (e > 0 && (head[e - 1] == ' ' || head[e - 1] == '\t')) --e;
    size_t nstart = e;
    while (nstart > 0 && is_ident_char(head[nstart - 1])) --nstart;
    if (nstart == e) return false;                  // no name token
    f.name = head.substr(nstart, e - nstart);
    std::string type_with_stars = trim(head.substr(0, nstart));
    if (type_with_stars.empty()) return false;

    // Pointer? count trailing '*' (and any between type and name).
    int stars = 0;
    std::string type = type_with_stars;
    while (!type.empty() && type.back() == '*') { ++stars; type.pop_back(); type = trim(type); }
    f.is_pointer = stars > 0;
    f.type = normalize_type(type_with_stars);

    bool dim_ok = true;
    {  // detect the unknown-dimension case set above
        size_t b = decl.find('[');
        if (b != std::string::npos) {
            // re-evaluate to know if any dim failed
            std::string dims = decl.substr(b);
            size_t i = 0; int prod = 1;
            while (i < dims.size()) {
                if (dims[i] == '[') {
                    size_t close = dims.find(']', i);
                    if (close == std::string::npos) { dim_ok = false; break; }
                    int d;
                    if (!eval_dim(dims.substr(i + 1, close - i - 1), d) || d <= 0) { dim_ok = false; break; }
                    prod *= d; i = close + 1;
                } else ++i;
            }
            if (dim_ok) count = prod;
        }
    }
    f.array_count = count;

    if (f.is_pointer) {
        f.pointee = normalize_type(type);
        f.kind = FKind::Ptr;
        f.sizable = dim_ok;                         // pointer (4/8) is always sizable
        return true;
    }

    FKind k;
    if (scalar_kind(normalize_type(type), k)) {
        f.kind = k;
        f.sizable = dim_ok;
        return true;
    }

    // Embedded value type / enum / unknown: recorded but not sizable.
    f.sizable = false;
    return true;
}

ParsedType parse_decomp_file(const std::string& path, const std::string& type_name) {
    std::ifstream in(path);
    if (!in) { ParsedType pt; pt.name = type_name; pt.found = false; return pt; }
    std::stringstream ss; ss << in.rdbuf();
    return parse_decomp_type(ss.str(), type_name);
}

std::vector<LField> to_lfields(const ParsedType& t) {
    std::vector<LField> out;
    for (const auto& f : t.fields) {
        if (!f.sizable) continue;   // caller should only use this on fully_sizable types
        out.push_back(LField{ f.name, f.kind, f.pointee, f.array_count });
    }
    return out;
}

EngineLayout to_engine_layout(const ParsedType& t, const std::set<std::string>& engine_types) {
    EngineLayout L;
    for (const auto& f : t.fields) {
        std::string nested;
        if (f.is_pointer && engine_types.count(f.pointee)) nested = f.pointee;
        L.fields[f.offset] = FieldDesc{ f.name, nested };
    }
    return L;
}
