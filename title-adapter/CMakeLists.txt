# Guest-layout extraction: GMSE01's own objects, read out of guest memory through a caller-supplied
# reader. It depends on `native-render` for the semantic types it fills in, and deliberately not on
# `gcnport` -- the reader is a parameter, so this builds and is tested without Dolphin.
add_library(sunbright-title-adapter STATIC src/guest_j3d_shape.cpp)
target_include_directories(sunbright-title-adapter PUBLIC include)
target_link_libraries(sunbright-title-adapter PUBLIC sunbright-native-render-core)

add_executable(title_adapter_guest_j3d_shape_test tests/guest_j3d_shape_test.cpp)
target_link_libraries(title_adapter_guest_j3d_shape_test PRIVATE sunbright-title-adapter)
add_test(NAME title_adapter_guest_j3d_shape COMMAND title_adapter_guest_j3d_shape_test)
