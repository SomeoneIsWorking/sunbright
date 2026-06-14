// Case-fold shim for a case-sensitive filesystem.
//
// reference/sms/include/JSystem/J3D/J3DGraphBase/J3DMaterial.hpp includes
//     <JSystem/J3d/J3DGraphBase/Blocks/J3DTevBlocks.hpp>   (lowercase "J3d")
// but the real header lives at
//     JSystem/J3D/J3DGraphBase/Blocks/J3DTevBlocks.hpp     (uppercase "J3D").
// CodeWarrior on Windows/macOS resolved this case-insensitively; Linux's
// case-sensitive FS does not. We cannot edit the pristine decomp, so this
// shadow (earlier on the -I path) satisfies the lowercase spelling and
// forwards to the real, correctly-cased header.
#include <JSystem/J3D/J3DGraphBase/Blocks/J3DTevBlocks.hpp>
