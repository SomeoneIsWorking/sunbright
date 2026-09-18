# Guest-layout extraction: GMSE01's own objects, read out of guest memory through a caller-supplied
# reader. It depends on `native-render` for the semantic types it fills in, and deliberately not on
# `gcnport` -- the reader is a parameter, so this builds and is tested without Dolphin.
add_library(sunbright-title-adapter STATIC
  src/guest_j3d_shape.cpp
  src/guest_j3d_pose.cpp
  src/guest_j3d_pixel_engine.cpp
  src/guest_j3d_color.cpp
  src/guest_j3d_texgen.cpp
  src/guest_j3d_tev.cpp
  src/guest_j3d_material.cpp
  src/guest_j3d_texture.cpp
)
target_include_directories(sunbright-title-adapter PUBLIC include)
target_link_libraries(sunbright-title-adapter PUBLIC sunbright-native-render-core)

foreach(title_adapter_test guest_j3d_shape guest_j3d_pose guest_j3d_pixel_engine
                          guest_j3d_color guest_j3d_texgen guest_j3d_tev
                          guest_j3d_material guest_j3d_texture)
  add_executable(title_adapter_${title_adapter_test}_test tests/${title_adapter_test}_test.cpp)
  target_link_libraries(title_adapter_${title_adapter_test}_test PRIVATE sunbright-title-adapter)
  add_test(NAME title_adapter_${title_adapter_test}
           COMMAND title_adapter_${title_adapter_test}_test)
endforeach()
