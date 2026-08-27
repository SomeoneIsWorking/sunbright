#include "frame_interp/tag_2d.h"

#include "frame_interp/populations.h"

#include <cstdint>
#include <cstdio>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);  \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

namespace {

u8 g_population = SB_POP_UNLABELLED;
bool g_populationAutomatic = false;
uint64_t g_tag = 0;
bool g_lerpEnabled = true;

} // namespace

void sbr_gxfifo_draw_pop(u8 population) {
    g_population = population;
    g_populationAutomatic = false;
}

void sbr_gxfifo_draw_pop_auto(u8 population) {
    g_population = population;
    g_populationAutomatic = true;
}

u8 sbr_gxfifo_pending_pop() {
    return g_population;
}

bool sbr_gxfifo_pending_pop_auto() {
    return g_populationAutomatic;
}

void sbr_gxfifo_draw_tag(uint64_t tag) {
    g_tag = tag;
}

uint64_t sbr_gxfifo_pending_tag() {
    return g_tag;
}

bool sbr_lerp_enabled() {
    return g_lerpEnabled;
}

int main() {
    using sb::frame_interp::two_d::DrawPath;
    using sb::frame_interp::two_d::PaneScope;

    sbr_gxfifo_draw_pop_auto(73);
    {
        PaneScope outer(0x80401234u, DrawPath::Picture);
        CHECK(g_population == SB_POP_J2D);
        CHECK(!g_populationAutomatic);
        CHECK(g_tag == 0x8040123400000001ull);

        PaneScope nested(0x80401234u, DrawPath::QuadEmitter);
        CHECK(g_population == SB_POP_J2D);
        CHECK(g_tag == 0x8040123400000001ull);
    }
    CHECK(g_tag == 0);
    CHECK(g_population == 73);
    CHECK(g_populationAutomatic);

    sbr_gxfifo_draw_pop(41);
    sbr_gxfifo_draw_tag(0x1234u);
    {
        PaneScope prelabelled(0x80405678u, DrawPath::QuadEmitter);
        CHECK(g_population == 41);
        CHECK(g_tag == 0x1234u);
    }
    CHECK(g_population == 41);
    CHECK(!g_populationAutomatic);
    CHECK(g_tag == 0x1234u);

    sbr_gxfifo_draw_tag(0);
    uint64_t pictureTag = 0;
    uint64_t quadTag = 0;
    {
        PaneScope picture(0x8040abcdu, DrawPath::Picture);
        pictureTag = g_tag;
    }
    {
        PaneScope quad(0x8040abcdu, DrawPath::QuadEmitter);
        quadTag = g_tag;
    }
    CHECK(pictureTag != 0);
    CHECK(quadTag != 0);
    CHECK(pictureTag != quadTag);

    uint64_t repeatedPictureTag = 0;
    {
        PaneScope repeatedPicture(0x8040abcdu, DrawPath::Picture);
        repeatedPictureTag = g_tag;
    }
    CHECK(repeatedPictureTag == pictureTag);

    g_lerpEnabled = false;
    sbr_gxfifo_draw_pop_auto(88);
    {
        PaneScope disabled(0x8040abcdu, DrawPath::Picture);
        CHECK(g_population == 88);
        CHECK(g_populationAutomatic);
        CHECK(g_tag == 0);
    }
}
