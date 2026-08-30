#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Native-platform bridge kept opaque so the decomp source does not depend on PC renderer types.
// The implementation copies the final GC2D fill_rect arguments into the ordered semantic frame.
void sb_native_solid_rectangle_submit(const void* rect, uint32_t rgba);

#ifdef __cplusplus
}
#endif
