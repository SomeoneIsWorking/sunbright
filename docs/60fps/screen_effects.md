# Screen-effect catalog

Every entry below is recovered from `decomp/sms` and scoped to exact `GMSE01`. The catalog preserves
semantic identities for future native hooks; it is not an active runtime inventory.

## Shared capture

`TScreenTexture` (`gpScreenTexture`, NameRef `スクリーンテクスチャ`) owns a half-resolution RGB565
image sized from half the game render width and height. The normal-scene EFB-control pass copies the
rendered image into it. Effects bind that resource through `TScreenTexture::replace` and
`JUTTexture::setResTIMG`.

`SMS_GetLightPerspectiveForEffectMtx` (`0x8022ba74`) builds the projected-texture matrix from camera
field of view and aspect. Its depth row is replaced so world position maps to the screen location
used to sample the capture. Widescreen must change the camera projection and this lookup coherently.

## Consumers

| Effect | Address | Capture/history contract |
|---|---:|---|
| `TShimmer::perform` | `0x8019f83c` | Shared capture plus indirect distortion; geometry and lookup projection must agree |
| `TAfterEffect::perform` | `0x8022d4f8` | Prior-frame dash trail; history advances once per game tick |
| `TModelWaterManager::drawRefracAndSpec` | `0x8027c12c` | Shared capture projected onto water geometry |
| `TBathWaterManager::draw_mist` | `0x801aa6cc` | Own EFB copy and pixel-space redraw |
| `TMirrorCamera::perform` | `0x80193fbc` | Separate scene render into a 256×256 texture |

The first three consumers can expose a mismatch between in-between geometry and captured pixels.
The bath and mirror paths own separate render/copy lifecycles and must not be treated as ordinary
uses of the shared capture.

## Native hook requirements

- Identify the effect by exact title function and object lifetime, not pixel sampling.
- Publish typed camera, geometry, resource, and history values into the native renderer.
- Count reached, admitted, refused, and unsupported cases.
- Preserve the authored history semantics of feedback effects.
- Prove a forced opposite result before trusting absence of an artifact.
