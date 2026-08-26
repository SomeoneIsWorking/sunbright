# TMapObjPlane is dormant in the retail stage set

`TMapObjPlane::draw` initially looked like an unobserved interpolation target: its decomp source
rebuilds direct-f32 position and normal strips from a height map that `depress()` can mutate. That
code shape is not evidence that retail content constructs the class.

The controlled census extracted all 108 files under `/data/scene/*.szs` from the configured US
retail disc, decompressed every Yaz0 archive to RARC, and searched the complete decompressed archive
bytes for the exact factory type strings `RockPlane` and `SandPlane`. Both counts were zero.

The scan had a positive control: the same decompression and byte search found the known serialized
scene strings `MapObjGrass` and `Mario` in `monte0.szs`. A clean result therefore did not come from
reading compressed bytes or from an empty archive set.

The source-side ownership agrees with the archive result. `MarNameRefGen_MapObj.cpp` constructs
`TRockPlane` and `TSandPlane` only after exact `strcmp` matches on those two strings, and no other
decomp source constructs either concrete class. Therefore no retail stage or scenario reaches
`TMapObjPlane::draw`: it is dormant retail code, not a missing lerp target. A hook for it would be
speculative coverage with no runtime falsifier, so none is retained.
