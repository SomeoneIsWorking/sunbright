add_library(sunbright-native-render-core STATIC src/frame.cpp src/image.cpp src/image_decode.cpp
                                                 src/picture.cpp src/picture_context.cpp
                                                 src/picture_sink.cpp src/semantic_frame_bridge.cpp)
target_include_directories(sunbright-native-render-core PUBLIC include)

add_executable(native_render_picture_test tests/picture_test.cpp)
target_link_libraries(native_render_picture_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_picture COMMAND native_render_picture_test)

add_executable(native_render_picture_sink_test tests/picture_sink_test.cpp)
target_link_libraries(native_render_picture_sink_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_picture_sink COMMAND native_render_picture_sink_test)

add_executable(native_render_image_decode_test tests/image_decode_test.cpp)
target_link_libraries(native_render_image_decode_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_image_decode COMMAND native_render_image_decode_test)

add_executable(native_render_frame_test tests/frame_test.cpp)
target_link_libraries(native_render_frame_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_frame COMMAND native_render_frame_test)

add_executable(native_render_picture_context_test tests/picture_context_test.cpp)
target_link_libraries(native_render_picture_context_test PRIVATE sunbright-native-render-core)
add_test(NAME native_render_picture_context COMMAND native_render_picture_context_test)

add_executable(native_render_semantic_frame_bridge_test
               tests/semantic_frame_bridge_test.cpp)
target_link_libraries(native_render_semantic_frame_bridge_test
                      PRIVATE sunbright-native-render-core)
add_test(NAME native_render_semantic_frame_bridge
         COMMAND native_render_semantic_frame_bridge_test)

add_library(sunbright-native-render-sdl3 STATIC src/picture_pass.cpp src/sdl_gpu_calls.cpp
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

# This test submits real GPU work and is intentionally not part of unguarded ctest. Run it through
# tools/render/gpu_watch.py so a driver/device fault is captured and the exact process group stops.
add_executable(native_render_picture_gpu_test tests/picture_pass_gpu_test.cpp)
target_link_libraries(native_render_picture_gpu_test PRIVATE sunbright-native-render-sdl3)
