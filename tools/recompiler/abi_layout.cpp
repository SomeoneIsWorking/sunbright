#include "abi_layout.h"

// See abi_layout.h for the GameCube/host layout rules this implements.

namespace {

void kind_size_align(FKind k, const ABIOpts& o, int& size, int& align) {
    switch (k) {
    case FKind::I8:  case FKind::U8:  size = 1; align = 1; break;
    case FKind::I16: case FKind::U16: size = 2; align = 2; break;
    case FKind::I32: case FKind::U32: case FKind::F32: size = 4; align = 4; break;
    case FKind::I64: case FKind::U64: size = 8; align = 8; break;
    case FKind::F64: size = 8; align = 8; break;
    case FKind::Ptr: size = o.ptr_size; align = o.ptr_align; break;
    }
}

int round_up(int v, int a) { return (v + a - 1) / a * a; }

}  // namespace

void field_size_align(const LField& f, const ABIOpts& o, int& size, int& align) {
    kind_size_align(f.kind, o, size, align);
    int count = f.array_count > 0 ? f.array_count : 1;
    size *= count;        // an array occupies count contiguous elements; align is the element's
}

LResult compute_layout(const std::vector<LField>& fields, bool polymorphic, const ABIOpts& o) {
    LResult r;
    r.offsets.reserve(fields.size());
    int cur = 0;
    int max_align = 1;

    if (polymorphic) {                 // vtable pointer at offset 0
        cur = o.ptr_size;
        max_align = o.ptr_align;
    }

    for (const auto& f : fields) {
        int size, align;
        kind_size_align(f.kind, o, size, align);
        int count = f.array_count > 0 ? f.array_count : 1;
        int off = round_up(cur, align);
        r.offsets.push_back(off);
        cur = off + size * count;        // an array occupies count contiguous elements
        if (align > max_align) max_align = align;
    }

    r.align = max_align;
    r.size = round_up(cur, max_align);
    return r;
}
