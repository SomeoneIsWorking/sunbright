# Authored mip chains reach the PC-native renderer

The four-image Mario masked-toon material was classified at the J3D shape boundary but did not
reach the native shader because the shared ResTIMG decoder rejected every sampler whose minimum
filter selected lower-resolution images. That was an incomplete asset boundary, not a GPU failure
or a reason to approximate the material with its base image.

`decode_res_timg` now reads the retail `mipmapCount`, validates the complete tiled source span,
decodes each level to ordinary RGBA8, and derives the immutable image revision from every decoded
source level. The decomp adapter supplies its loader-normalized count; the recomp adapter reads the
same field from the guest header. `SdlImageCache` creates the matching number of SDL texture levels,
uploads each one, and makes sampler cache identity and maximum LOD depend on that count. Image
producers that provide only a base level keep their established base-level behavior even when a
material requests mip filtering.

The decoder control uses a 4×4 red RGB565 level and a 2×2 blue lower level, verifies both guest and
native-layout headers, and refuses a mip-filtering header that claims only one level. The guarded
shipping GPU control minifies a red 4×4 image to the authored blue lower level, then proves a
base-only image remains red rather than being rejected. The guarded 120-present recomp audit exited
normally with no kernel GPU fault and advanced the named four-image material through all 50
observations, image decodes, perspective-ready calls, and PC-native model submissions.
