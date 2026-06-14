#include "func_sig.h"

#include <cctype>
#include <fstream>
#include <sstream>

// See func_sig.h. Focused GNU-v2 ("ARM"/CodeWarrior) demangler — only the part needed to
// know which registers hold engine-object pointers on entry. Not a full demangler.

namespace {

std::string leaf_of(const std::string& qualified) {
    size_t p = qualified.rfind("::");
    return p == std::string::npos ? qualified : qualified.substr(p + 2);
}

// Read a decimal length then that many chars at s[pos]; advances pos. "" on failure.
std::string read_len_name(const std::string& s, size_t& pos) {
    size_t start = pos;
    long n = 0; bool any = false;
    while (pos < s.size() && std::isdigit((unsigned char)s[pos])) { n = n * 10 + (s[pos]-'0'); ++pos; any = true; }
    if (!any || pos + (size_t)n > s.size()) { pos = start; return ""; }
    std::string name = s.substr(pos, n);
    pos += n;
    return name;
}

// Parse a qualified class token after the 'Q': Q<count><part1><part2>... -> "a::b::c".
std::string read_qualified(const std::string& s, size_t& pos) {
    if (pos >= s.size() || s[pos] != 'Q') return "";
    ++pos;
    if (pos >= s.size() || !std::isdigit((unsigned char)s[pos])) return "";
    int count = s[pos] - '0'; ++pos;             // single-digit part count (sufficient here)
    std::string out;
    for (int i = 0; i < count; ++i) {
        std::string part = read_len_name(s, pos);
        if (part.empty()) return "";
        if (!out.empty()) out += "::";
        out += part;
    }
    return out;
}

// Result of consuming one argument type.
struct ArgT {
    bool ok = true;          // parsed cleanly
    bool is_float = false;   // bare float/double -> FPR, consumes NO GPR
    int  gpr_words = 1;      // GPRs consumed (0 float/void, 2 long long, -1 = unknown by-value)
    std::string klass;       // pointer/ref-to-class -> qualified class name; else ""
};

// Consume one type starting at pos; advances pos. Honest about by-value structs / backrefs
// (gpr_words = -1) so the caller can stop assigning rather than guess.
ArgT consume_type(const std::string& s, size_t& pos) {
    ArgT a;
    bool ptr_or_ref = false;
    // prefixes
    while (pos < s.size()) {
        char c = s[pos];
        if (c == 'P' || c == 'R') { ptr_or_ref = true; ++pos; }
        else if (c == 'C' || c == 'V' || c == 'U' || c == 'S' || c == 'u') { ++pos; }  // cv/sign
        else break;
    }
    if (pos >= s.size()) { a.ok = false; return a; }
    char c = s[pos];
    if (c == 'Q' || std::isdigit((unsigned char)c)) {                 // class type
        std::string q = (c == 'Q') ? read_qualified(s, pos) : read_len_name(s, pos);
        if (q.empty()) { a.ok = false; return a; }
        if (ptr_or_ref) { a.klass = q; a.gpr_words = 1; }
        else { a.gpr_words = -1; }                                    // by-value struct: unknown
        return a;
    }
    ++pos;                                                            // builtin: consume the letter
    switch (c) {
    case 'v': a.gpr_words = ptr_or_ref ? 1 : 0; break;                // void / void*
    case 'f': case 'd': if (ptr_or_ref) a.gpr_words = 1; else { a.is_float = true; a.gpr_words = 0; } break;
    case 'x':           a.gpr_words = ptr_or_ref ? 1 : 2; break;      // long long (2 GPRs by value)
    case 'i': case 'l': case 's': case 'c': case 'b': case 'w':
                        a.gpr_words = 1; break;
    case 'e':           a.ok = false; break;                          // ellipsis: stop
    case 'T': case 'N': case 'M': a.ok = false; break;                // backref / ptr-to-member
    default:            a.ok = false; break;
    }
    return a;
}

}  // namespace

FuncSig demangle_signature(const std::string& mangled) {
    FuncSig sig;
    // Find the "__" that introduces the class scope or the free-function 'F' signature: the
    // first "__" followed by a digit, 'Q', or 'F' (skips operator names like __ct/__as).
    size_t split = std::string::npos;
    for (size_t i = 0; i + 2 < mangled.size(); ++i) {
        if (mangled[i] == '_' && mangled[i+1] == '_') {
            char n = mangled[i+2];
            if (std::isdigit((unsigned char)n) || n == 'Q' || n == 'F') { split = i; break; }
        }
    }
    if (split == std::string::npos) return sig;     // not a mangled C++ symbol we handle
    size_t pos = split + 2;

    if (mangled[pos] != 'F') {                       // method: parse the class scope
        std::string klass = (mangled[pos] == 'Q') ? read_qualified(mangled, pos)
                                                   : read_len_name(mangled, pos);
        if (klass.empty()) return sig;
        sig.is_method = true;
        sig.class_name = klass;
        sig.class_leaf = leaf_of(klass);
        while (pos < mangled.size() && (mangled[pos] == 'C' || mangled[pos] == 'V')) ++pos; // cv method
    }
    if (pos >= mangled.size() || mangled[pos] != 'F') return sig;     // expect the 'F'
    ++pos;

    int gpr = 3;
    if (sig.is_method) {                              // `this` in r3 (instance-method assumption)
        sig.ptr_args.push_back(SigArg{ 3, sig.class_leaf, sig.class_name });
        gpr = 4;
    }
    // walk the argument list, assigning GPR slots; floats/doubles use FPRs (no GPR)
    while (pos < mangled.size()) {
        if (mangled[pos] == 'v' && pos + 1 == mangled.size()) break;  // trailing 'v' = (void)
        ArgT a = consume_type(mangled, pos);
        if (!a.ok || a.gpr_words < 0) { sig.ambiguous = true; break; } // stop: can't place the rest
        if (a.is_float || a.gpr_words == 0) continue;                  // FPR / void: no GPR
        if (gpr > 10) break;                                           // ran out of GPR arg slots
        if (!a.klass.empty()) sig.ptr_args.push_back(SigArg{ gpr, leaf_of(a.klass), a.klass });
        gpr += a.gpr_words;
    }
    sig.ok = true;
    return sig;
}

std::map<u32, std::map<int, std::string>>
build_signatures(const std::string& funcs_txt_path, const std::set<std::string>& engine_types) {
    std::map<u32, std::map<int, std::string>> out;
    std::ifstream in(funcs_txt_path);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string addr_s, name;
        if (!(ls >> addr_s >> name)) continue;
        u32 addr;
        try { addr = (u32)std::stoul(addr_s, nullptr, 16); } catch (...) { continue; }
        FuncSig sig = demangle_signature(name);
        if (!sig.ok) continue;
        for (const auto& a : sig.ptr_args)
            if (engine_types.count(a.type)) out[addr][a.gpr] = a.type;
    }
    return out;
}
