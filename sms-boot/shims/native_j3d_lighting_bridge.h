#pragma once

struct SbNativeJ3dStageLighting {
    float view[12]{};
    float primaryWorldPosition[3]{};
    unsigned char primaryRgba[4]{};
    float shininess = 1.0F;
    unsigned char ambientRgba[4]{};
    unsigned char effectEnabled = 0;
    float effectWorldPosition[3]{};
    unsigned char effectRgba[4]{};
};

extern "C" void sb_native_j3d_publish_stage_lighting(const SbNativeJ3dStageLighting* lighting);
