layout(location = 5) in float model_eye_depth;

layout(set = 3, binding = 0) uniform ModelRasterBlock {
    vec4 alpha_test;
    vec4 fog_range;
    vec4 fog_color;
};

vec4 apply_model_raster(vec4 color) {
    if (color.a < alpha_test.x) {
        discard;
    }
    float fog_amount = clamp((model_eye_depth - fog_range.x) * fog_range.y, 0.0, fog_range.z);
    color.rgb = mix(color.rgb, fog_color.rgb, fog_amount);
    return color;
}
