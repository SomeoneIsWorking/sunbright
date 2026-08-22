// overrides.cpp — the override registry.

#include "overrides.h"

#include "guest_address_table.h"

#include <lucent/log.h>

#include <deque>

namespace {

struct Entry {
    void (*fn)(CPUState&);
    const char* symbol;
    const char* reason;
    bool announced;
};

// Function-local statics: registration happens from static initializers in the per-override
// translation units, so both containers must be constructed on first use. deque keeps Entry
// addresses stable while the sparse table stores pointers to them.
GuestAddressTable<Entry>& table() {
    static GuestAddressTable<Entry> t;
    return t;
}

std::deque<Entry>& entries() {
    static std::deque<Entry> value;
    return value;
}

} // namespace

void override_register(u32 address, void (*fn)(CPUState&), const char* symbol, const char* reason) {
    // TWO OVERRIDES FOR ONE ADDRESS IS ALWAYS A BUG, and it used to resolve itself silently: the
    // map assignment overwrote, so whichever translation unit's static initializer ran last won and
    // the other's behaviour vanished with no diagnostic. Both are real work someone did to that
    // function; losing either is a defect that presents as "my hook never runs" or, far worse, as
    // the OTHER hook's fix quietly disappearing. Registration order across TUs is not something to
    // rely on either way. The fix is always to merge the two bodies into one override.
    if (const Entry* existing = table().find(address); existing != nullptr) {
        lucent::error("override",
                      "DUPLICATE OVERRIDE for 0x{:08x}: '{}' ({}) is already registered, "
                      "and '{}' ({}) would silently replace it. One address, one "
                      "override — merge the two bodies.",
                      address, existing->symbol, existing->reason, symbol, reason);
        std::abort();
    }
    entries().push_back(Entry{fn, symbol, reason, false});
    if (!table().insert(address, &entries().back())) {
        lucent::error("override", "override address 0x{:08x} is outside aligned MEM1 code space",
                      address);
        std::abort();
    }
}

// Is this function overridden? A QUERY, and deliberately not override_lookup(): that one announces
// the override on first use, which is a statement that the game executed it. Something merely
// asking (the graphics registry, when it seeds a row's RE hint) must not put that line in the log.
bool override_exists(u32 address) {
    return table().find(address) != nullptr;
}

void (*override_lookup(u32 address))(CPUState&) {
    Entry* entry = table().find(address);
    if (entry == nullptr)
        return nullptr;
    // Announce once. An override changes what the game actually executes, so it must be
    // visible in the log without needing a debug channel enabled — a silently swapped
    // function is indistinguishable from a working one when behaviour later goes wrong.
    if (!entry->announced) {
        entry->announced = true;
        lucent::info("override", "0x{:08x} {} -> native ({})", address, entry->symbol,
                     entry->reason);
    }
    return entry->fn;
}
