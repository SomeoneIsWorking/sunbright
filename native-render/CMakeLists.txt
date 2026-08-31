add_library(sunbright-native-render-core STATIC src/frame.cpp src/glyph.cpp src/image.cpp src/image_decode.cpp
                                                 src/j3d_effect_material.cpp
                                                 src/j3d_dual_alpha_effect_material.cpp
                                                 src/j3d_alpha_masked_material.cpp
                                                 src/j3d_lit_alpha_mask_material.cpp
                                                 src/j3d_lit_alpha_tint_material.cpp
                                                 src/j3d_layered_material.cpp
                                                 src/j3d_lit_material.cpp
                                                 src/j3d_masked_toon_material.cpp
                                                 src/j3d_fog.cpp
                                                 src/j3d_mesh_decode.cpp
                                                 src/j3d_specular_material.cpp
                                                 src/j3d_tinted_layered_material.cpp
                                                 src/j3d_stage_lighting.cpp
                                                 src/j3d_unlit_material.cpp
                                                 src/model.cpp src/model_context.cpp
                                                 src/res_timg_decode.cpp
                                                 src/picture.cpp src/picture_context.cpp
                                                 src/semantic_2d_types.cpp
                                                 src/semantic_frame_mode.cpp
                                                 src/semantic_frame_bridge.cpp src/semantic_sink.cpp
                                                 src/solid_rectangle.cpp src/window.cpp)
target_include_directories(sunbright-native-render-core PUBLIC include)

add_executable(native_render_glyph_test tests/glyph_test.cpp)
target_link_libraries(native_render_glyph_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_glyph COMMAND native_render_glyph_test)

add_executable(native_render_picture_test tests/picture_test.cpp)
target_link_libraries(native_render_picture_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_picture COMMAND native_render_picture_test)

add_executable(native_render_window_test tests/window_test.cpp)
target_link_libraries(native_render_window_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_window COMMAND native_render_window_test)

add_executable(native_render_semantic_sink_test tests/semantic_sink_test.cpp)
target_link_libraries(native_render_semantic_sink_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_semantic_sink COMMAND native_render_semantic_sink_test)

add_executable(native_render_image_decode_test tests/image_decode_test.cpp)
target_link_libraries(native_render_image_decode_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_image_decode COMMAND native_render_image_decode_test)

add_executable(native_render_frame_test tests/frame_test.cpp)
target_link_libraries(native_render_frame_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_frame COMMAND native_render_frame_test)

add_executable(native_render_picture_context_test tests/picture_context_test.cpp)
target_link_libraries(native_render_picture_context_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_picture_context COMMAND native_render_picture_context_test)

add_executable(native_render_model_test tests/model_test.cpp)
target_link_libraries(native_render_model_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_model COMMAND native_render_model_test)

add_executable(native_render_model_context_test tests/model_context_test.cpp)
target_link_libraries(native_render_model_context_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_model_context COMMAND native_render_model_context_test)

add_executable(native_render_j3d_mesh_decode_test tests/j3d_mesh_decode_test.cpp)
target_link_libraries(native_render_j3d_mesh_decode_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_mesh_decode COMMAND native_render_j3d_mesh_decode_test)

add_executable(native_render_j3d_fog_test tests/j3d_fog_test.cpp)
target_link_libraries(native_render_j3d_fog_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_fog COMMAND native_render_j3d_fog_test)

add_executable(native_render_j3d_unlit_material_test tests/j3d_unlit_material_test.cpp)
target_link_libraries(native_render_j3d_unlit_material_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_unlit_material COMMAND native_render_j3d_unlit_material_test)

add_executable(native_render_j3d_effect_material_test tests/j3d_effect_material_test.cpp)
target_link_libraries(native_render_j3d_effect_material_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_effect_material COMMAND native_render_j3d_effect_material_test)

add_executable(native_render_j3d_dual_alpha_effect_material_test
               tests/j3d_dual_alpha_effect_material_test.cpp)
target_link_libraries(native_render_j3d_dual_alpha_effect_material_test
                      PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_dual_alpha_effect_material
         COMMAND native_render_j3d_dual_alpha_effect_material_test)

add_executable(native_render_j3d_alpha_masked_material_test tests/j3d_alpha_masked_material_test.cpp)
target_link_libraries(native_render_j3d_alpha_masked_material_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_alpha_masked_material COMMAND native_render_j3d_alpha_masked_material_test)

add_executable(native_render_j3d_lit_alpha_mask_material_test tests/j3d_lit_alpha_mask_material_test.cpp)
target_link_libraries(native_render_j3d_lit_alpha_mask_material_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_lit_alpha_mask_material COMMAND native_render_j3d_lit_alpha_mask_material_test)

add_executable(native_render_j3d_lit_alpha_tint_material_test
               tests/j3d_lit_alpha_tint_material_test.cpp)
target_link_libraries(native_render_j3d_lit_alpha_tint_material_test
                      PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_lit_alpha_tint_material
         COMMAND native_render_j3d_lit_alpha_tint_material_test)

add_executable(native_render_j3d_layered_material_test tests/j3d_layered_material_test.cpp)
target_link_libraries(native_render_j3d_layered_material_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_layered_material COMMAND native_render_j3d_layered_material_test)

add_executable(native_render_j3d_tinted_layered_material_test
               tests/j3d_tinted_layered_material_test.cpp)
target_link_libraries(native_render_j3d_tinted_layered_material_test
                      PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_tinted_layered_material
         COMMAND native_render_j3d_tinted_layered_material_test)

add_executable(native_render_j3d_masked_toon_material_test
               tests/j3d_masked_toon_material_test.cpp)
target_link_libraries(native_render_j3d_masked_toon_material_test
                      PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_masked_toon_material
         COMMAND native_render_j3d_masked_toon_material_test)

add_executable(native_render_j3d_lit_material_test tests/j3d_lit_material_test.cpp)
target_link_libraries(native_render_j3d_lit_material_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_lit_material COMMAND native_render_j3d_lit_material_test)

add_executable(native_render_j3d_specular_material_test tests/j3d_specular_material_test.cpp)
target_link_libraries(native_render_j3d_specular_material_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_j3d_specular_material COMMAND native_render_j3d_specular_material_test)

add_executable(native_render_res_timg_decode_test tests/res_timg_decode_test.cpp)
target_link_libraries(native_render_res_timg_decode_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_res_timg_decode COMMAND native_render_res_timg_decode_test)

add_executable(native_render_semantic_frame_bridge_test
               tests/semantic_frame_bridge_test.cpp)
target_link_libraries(native_render_semantic_frame_bridge_test
                      PRIVATE sunbright-native-render-core)
add_test(NAME native_render_semantic_frame_bridge
         COMMAND native_render_semantic_frame_bridge_test)

add_library(sunbright-native-render-sdl3 STATIC src/semantic_2d_pass.cpp src/semantic_3d_pass.cpp src/sdl_image_cache.cpp src/sdl_gpu_calls.cpp
                                                src/sdl_gpu_error.cpp
                                                src/sdl_gpu_frame_target.cpp
                                                src/sdl_gpu_platform.cpp
                                                src/sdl_gpu_presenter.cpp
                                                src/sdl_semantic_frame_client.cpp)
target_include_directories(sunbright-native-render-sdl3 PUBLIC include)
target_link_libraries(sunbright-native-render-sdl3 PUBLIC sunbright-native-render-core SDL3::SDL3)

add_executable(native_render_sdl_gpu_platform_test tests/sdl_gpu_platform_test.cpp)
target_link_libraries(native_render_sdl_gpu_platform_test PRIVATE sunbright-native-render-sdl3)
add_test(NAME native_render_sdl_gpu_platform COMMAND native_render_sdl_gpu_platform_test)

add_executable(native_render_semantic_frame_mode_test tests/semantic_frame_mode_test.cpp)
target_link_libraries(native_render_semantic_frame_mode_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_semantic_frame_mode COMMAND native_render_semantic_frame_mode_test)

# This test submits real GPU work and is intentionally not part of unguarded ctest. Run it through
# tools/render/gpu_watch.py so a driver/device fault is captured and the exact process group stops.
add_executable(native_render_semantic_2d_gpu_test tests/semantic_2d_pass_gpu_test.cpp)
target_link_libraries(native_render_semantic_2d_gpu_test PRIVATE sunbright-native-render-sdl3)
