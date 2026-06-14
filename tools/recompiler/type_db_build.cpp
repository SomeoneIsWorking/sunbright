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

}  // namespace

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
    EngineLayout L;
    // Walk the single-inheritance chain root-first so derived fields win on any offset clash.
    std::vector<ParsedType> chain;
    std::set<std::string> seen;
    std::string cur = type_name;
    while (!cur.empty() && !seen.count(cur)) {
        seen.insert(cur);
        auto it = index.find(cur);
        if (it == index.end()) { if (unresolved) unresolved->push_back(cur); break; }
        ParsedType t = parse_decomp_file(it->second, cur);
        if (!t.found) { if (unresolved) unresolved->push_back(cur); break; }
        chain.push_back(t);
        cur = t.base;
    }
    // Union fields by annotated (object-absolute) offset, base-most first.
    for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
        EngineLayout one = to_engine_layout(*rit, engine_types);
        for (const auto& [off, fd] : one.fields) L.fields[off] = fd;
    }
    return L;
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
