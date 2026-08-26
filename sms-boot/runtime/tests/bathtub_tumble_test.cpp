// Unit test from RE for TBathtub::tumble (GMSE01 0x801fb568).
// The test drives the real linked member function. A small deterministic JMath
// table makes sine/cosine orientation and short-angle indexing independently
// observable without initializing the game runtime.

#include <MoveBG/MapObjCorona.hpp>
#include <cstdio>
#include <cstring>

u32 jmaSinShift;
f32* jmaSinTable;
f32* jmaCosTable;

static int g_fail = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
            g_fail = 1;                                                                            \
        } else {                                                                                   \
            std::fprintf(stderr, "ok:   %s\n", msg);                                               \
        }                                                                                          \
    } while (0)

static TBathtub* reset_bathtub() {
    alignas(TBathtub) static unsigned char storage[sizeof(TBathtub)];
    std::memset(storage, 0, sizeof(storage));
    return reinterpret_cast<TBathtub*>(storage);
}

int main() {
    // Four-entry quarter-turn table: cos is the same allocation offset by 90°,
    // matching JMANewSinTable. Shift 14 maps 0/90/180/270° to indices 0/1/2/3.
    static f32 sin_table[] = {0.0f, 1.0f, 0.0f, -1.0f, 0.0f};
    jmaSinShift = 14;
    jmaSinTable = sin_table;
    jmaCosTable = sin_table + 1;

    TBathtub* bathtub = reset_bathtub();
    bathtub->unk1E8 = 2.0f;
    bathtub->unk1EC = -0.0f;
    bathtub->unk1F0 = 3.0f;
    bathtub->tumble(0.0f, 10000.0f);
    CHECK(bathtub->unk1E8 == 2.0f, "zero degrees has no X impulse");
    CHECK(bathtub->unk1EC == 0.0f, "Y remains numerically zero");
    CHECK(bathtub->unk1F0 == 2.0f, "zero degrees applies negative cosine to Z");

    bathtub = reset_bathtub();
    bathtub->unk1E8 = 2.0f;
    bathtub->unk1EC = 4.0f;
    bathtub->unk1F0 = 3.0f;
    bathtub->tumble(90.0f, 2500.0f);
    CHECK(bathtub->unk1E8 == 2.25f, "90 degrees applies scaled sine to X");
    CHECK(bathtub->unk1EC == 4.0f, "tumble does not translate Y");
    CHECK(bathtub->unk1F0 == 3.0f, "90 degrees has no Z impulse");

    // Negative control: byte 0x29A gates the whole function in the retail body.
    bathtub = reset_bathtub();
    bathtub->unk29A = 1;
    bathtub->unk1E8 = 2.0f;
    bathtub->unk1EC = 4.0f;
    bathtub->unk1F0 = 3.0f;
    bathtub->tumble(45.0f, 10000.0f);
    CHECK(bathtub->unk1E8 == 2.0f, "disabled tumble preserves X");
    CHECK(bathtub->unk1EC == 4.0f, "disabled tumble preserves Y");
    CHECK(bathtub->unk1F0 == 3.0f, "disabled tumble preserves Z");

    std::fprintf(stderr, g_fail ? "\nRESULT: FAIL\n" : "\nRESULT: PASS\n");
    return g_fail;
}
