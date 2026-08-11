// overrides.cpp — the override registry.

#include "overrides.h"

#include <lucent/log.h>

#include <unordered_map>

namespace {

struct Entry {
    void (*fn)(CPUState&);
    const char* symbol;
    const char* reason;
    bool announced;
};

// Function-local static: registration happens from static initializers in the
// per-override translation units, so the map must be constructed on first use rather
// than relying on cross-TU initialization order.
std::unordered_map<u32, Entry>& table() {
    static std::unordered_map<u32, Entry> t;
    return t;
}

// The registry is consulted on EVERY call_ppc. A hash lookup per call is measurable, so
// keep a cheap reject: overrides are a handful of addresses, and the min/max bounds
// throw out the overwhelming majority without touching the map at all.
u32 g_lo = 0xFFFFFFFFu, g_hi = 0;

} // namespace

void override_register(u32 address, void (*fn)(CPUState&), const char* symbol,
                       const char* reason) {
    // TWO OVERRIDES FOR ONE ADDRESS IS ALWAYS A BUG, and it used to resolve itself silently: the
    // map assignment overwrote, so whichever translation unit's static initializer ran last won and
    // the other's behaviour vanished with no diagnostic. Both are real work someone did to that
    // function; losing either is a defect that presents as "my hook never runs" or, far worse, as
    // the OTHER hook's fix quietly disappearing. Registration order across TUs is not something to
    // rely on either way. The fix is always to merge the two bodies into one override.
    const auto existing = table().find(address);
    if (existing != table().end()) {
        lucent::error("override", "DUPLICATE OVERRIDE for 0x{:08x}: '{}' ({}) is already registered, "
                                  "and '{}' ({}) would silently replace it. One address, one "
                                  "override — merge the two bodies.",
                      address, existing->second.symbol, existing->second.reason, symbol, reason);
        std::abort();
    }
    table()[address] = Entry{fn, symbol, reason, false};
    if (address < g_lo) g_lo = address;
    if (address > g_hi) g_hi = address;
}

// Is this function overridden? A QUERY, and deliberately not override_lookup(): that one announces
// the override on first use, which is a statement that the game executed it. Something merely
// asking (the graphics registry, when it seeds a row's RE hint) must not put that line in the log.
bool override_exists(u32 address) {
    if (address < g_lo || address > g_hi) return false;
    return table().find(address) != table().end();
}

void (*override_lookup(u32 address))(CPUState&) {
    if (address < g_lo || address > g_hi) return nullptr;
    auto it = table().find(address);
    if (it == table().end()) return nullptr;
    // Announce once. An override changes what the game actually executes, so it must be
    // visible in the log without needing a debug channel enabled — a silently swapped
    // function is indistinguishable from a working one when behaviour later goes wrong.
    if (!it->second.announced) {
        it->second.announced = true;
        lucent::info("override", "0x{:08x} {} -> native ({})", address, it->second.symbol,
                     it->second.reason);
    }
    return it->second.fn;
}
