#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Native-platform bridge kept opaque so the retail/decomp declaration does not depend on the PC
// renderer's C++ types. The implementation reads J2DPicture fields directly and publishes the same
// semantic command as the recomp adapter.
void sb_native_picture_submit(const void* picture, const void* parent_matrix);
void sb_native_picture_context_push(const void* graf_context, int clip_enabled);
void sb_native_picture_context_pop(void);

#ifdef __cplusplus
}
#endif
