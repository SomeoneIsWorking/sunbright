#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Opaque native-platform bridges: the decomp remains independent of PC renderer types while its
// high-level J2D entry points publish renderer-neutral pictures, text, and canvas context.
void sb_native_picture_submit(const void* picture, const void* parent_matrix);
void sb_native_picture_submit_direct(const void* picture, const void* position_matrix, int width,
                                     int height, int mirror_horizontal, int mirror_vertical,
                                     int transpose);
void sb_native_picture_context_push(const void* graf_context, int clip_enabled);
void sb_native_picture_context_pop(void);
void sb_native_picture_context_activate(const void* graf_context);
void sb_native_j2d_fill_box_submit(const void* graf_context, const void* rect);

void sb_native_text_context_push(const void* clip_rect, const void* transform);
void sb_native_text_context_pop(void);
void sb_native_font_remap(unsigned int black, unsigned int white);

typedef struct SbNativeFontGlyph {
    const void* instance;
    unsigned int code;
    int apply_bearing;
    float position_x;
    float position_y;
    float scale_x;
    float scale_y;
    unsigned int font_width;
    unsigned int font_height;
    unsigned int ascent;
    unsigned int descent;
    unsigned int left_bearing;
    unsigned int glyph_width;
    unsigned int fixed_width;
    int fixed;
    unsigned int cell_x;
    unsigned int cell_y;
    const void* atlas;
    unsigned int atlas_width;
    unsigned int atlas_height;
    unsigned int atlas_format;
    unsigned int atlas_size;
    unsigned int corner[4];
} SbNativeFontGlyph;

void sb_native_font_glyph_submit(const SbNativeFontGlyph* glyph);

#ifdef __cplusplus
}
#endif
