#include "guest_address_table.h"

#include <cstdio>

namespace {

void low() {}
void high() {}
void replacement() {}

bool expect(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "guest_address_table_test: %s\n", message);
    return condition;
}

} // namespace

int main() {
    GuestAddressTable<void()> table;

    bool ok = true;
    ok &= expect(table.find(0x80000000u) == nullptr, "an empty valid slot must miss");
    ok &= expect(table.insert(0x80000000u, &low), "the first aligned MEM1 address must insert");
    ok &= expect(table.insert(0x817ffffcu, &high), "the last aligned MEM1 address must insert");
    ok &= expect(table.find(0x80000000u) == &low, "the first address must return its exact value");
    ok &= expect(table.find(0x817ffffcu) == &high, "the last address must return its exact value");

    ok &= expect(!table.insert(0x80000000u, &replacement), "a duplicate address must be rejected");
    ok &= expect(table.find(0x80000000u) == &low, "a rejected duplicate must preserve the owner");
    ok &= expect(!table.insert(0x80000002u, &replacement), "a misaligned address must be rejected");
    ok &= expect(table.find(0x80000002u) == nullptr, "a misaligned lookup must miss");
    ok &=
        expect(!table.insert(0x7ffffffcu, &replacement), "an address below MEM1 must be rejected");
    ok &=
        expect(!table.insert(0x81800000u, &replacement), "an address above MEM1 must be rejected");
    ok &= expect(table.find(0xc0000000u) == nullptr,
                 "an uncached alias is not an exact cached address");

    return ok ? 0 : 1;
}
