add_library(sunbright-native-render-core STATIC src/image.cpp src/image_decode.cpp src/picture.cpp
                                                 src/picture_sink.cpp)
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

add_library(sunbright-native-render-sdl3 STATIC src/picture_pass.cpp)
target_include_directories(sunbright-native-render-sdl3 PUBLIC include)
target_link_libraries(sunbright-native-render-sdl3 PUBLIC sunbright-native-render-core SDL3::SDL3)

# This test submits real GPU work and is intentionally not part of unguarded ctest. Run it through
# tools/render/gpu_watch.py so a driver/device fault is captured and the exact process group stops.
add_executable(native_render_picture_gpu_test tests/picture_pass_gpu_test.cpp)
target_link_libraries(native_render_picture_gpu_test PRIVATE sunbright-native-render-sdl3)
