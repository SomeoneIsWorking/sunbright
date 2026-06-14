#include "type_db.h"

EngineLayout build_engine_layout(const std::vector<LField>& fields, bool polymorphic) {
    EngineLayout layout;
    LResult g = compute_layout(fields, polymorphic, guest_abi());
    for (size_t i = 0; i < fields.size(); ++i)
        layout.fields[g.offsets[i]] = FieldDesc{ fields[i].name, fields[i].nested_type };
    return layout;
}
