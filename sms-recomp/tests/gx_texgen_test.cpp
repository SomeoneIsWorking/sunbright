#include "../runtime/render/gx_texgen.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void check_near(float got, float want, const char* what) {
    if (std::fabs(got - want) <= 1.0f / 4096.0f)
        return;
    std::printf("FAIL: %s — got %.6f want %.6f\n", what, got, want);
    ++g_failures;
}

SbrGeomVert vertex() {
    SbrGeomVert v{};
    v.x = 2.0f;
    v.y = 3.0f;
    v.z = 4.0f;
    v.nx = 0.25f;
    v.ny = 0.5f;
    v.nz = 0.75f;
    v.uv[0][0] = 0.125f;
    v.uv[0][1] = 0.875f;
    return v;
}

void test_identity_uses_selected_source() {
    SbrXfState xf{};
    xf.texGen[0].sourceRow = 5;
    xf.texGen[0].mtxSlot = 0xff;
    const float raster[4] = {0, 0, 0, 1};
    float out[2]{};
    sbr_texgen(xf, 0, vertex(), raster, out);
    check_near(out[0], 0.125f, "identity TEX0 s");
    check_near(out[1], 0.875f, "identity TEX0 t");
}

void test_written_matrix_transforms_position() {
    SbrXfState xf{};
    xf.texGen[0].sourceRow = 0;
    xf.texGen[0].inputForm = 1;
    xf.texGen[0].mtxSlot = 2;
    xf.texMtxWritten = 1u << 2;
    float* matrix = xf.texMtx[2];
    matrix[0] = 2.0f;
    matrix[3] = 1.0f;
    matrix[5] = 3.0f;
    matrix[7] = -2.0f;
    const float raster[4] = {0, 0, 0, 1};
    float out[2]{};
    sbr_texgen(xf, 0, vertex(), raster, out);
    check_near(out[0], 5.0f, "2x4 matrix transforms position s");
    check_near(out[1], 7.0f, "2x4 matrix transforms position t");
}

void test_projective_matrix_divides_by_q() {
    SbrXfState xf{};
    xf.texGen[0].sourceRow = 0;
    xf.texGen[0].inputForm = 1;
    xf.texGen[0].projection = 1;
    xf.texGen[0].mtxSlot = 1;
    xf.texMtxWritten = 1u << 1;
    float* matrix = xf.texMtx[1];
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[11] = 2.0f;
    const float raster[4] = {0, 0, 0, 1};
    float out[2]{};
    sbr_texgen(xf, 0, vertex(), raster, out);
    check_near(out[0], 1.0f, "3x4 matrix divides s by q");
    check_near(out[1], 1.5f, "3x4 matrix divides t by q");
}

void test_color_texgen_clamps_raster_channel() {
    SbrXfState xf{};
    xf.texGen[0].type = 2;
    const float raster[4] = {-0.25f, 1.25f, 0, 1};
    float out[2]{};
    sbr_texgen(xf, 0, vertex(), raster, out);
    check_near(out[0], 0.0f, "COLOR0 clamps low s");
    check_near(out[1], 1.0f, "COLOR0 clamps high t");
}

} // namespace

int main() {
    test_identity_uses_selected_source();
    test_written_matrix_transforms_position();
    test_projective_matrix_divides_by_q();
    test_color_texgen_clamps_raster_channel();
    if (g_failures == 0) {
        std::printf("gx_texgen: all checks passed\n");
        return 0;
    }
    std::printf("gx_texgen: %d check(s) FAILED\n", g_failures);
    return 1;
}
