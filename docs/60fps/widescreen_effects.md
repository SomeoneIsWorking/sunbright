# Widescreen effect ownership

Widescreen is a deterministic projection and presentation feature. It widens the horizontal camera
projection and viewport while preserving source geometry, UVs, depth, color, and authored effect
history. Final-image stretching and content-dependent sampling are not acceptable substitutes.

## Recovered anchors

| Owner | Address | Required policy |
|---|---:|---|
| `C_MTXPerspective` | `0x8034a404` | Widen the shared camera-aspect input once |
| `GXSetProjection` | `0x80362c34` | Observe the title's selected projection at the host boundary |
| `SetViewFrustumClipCheckPerspective` | `0x802260cc` | Use the same widened aspect for horizontal culling |
| `TSMSFader::drawFadeinout` | `0x8013fa54` | Cover the complete output viewport |
| GC2D `fill_rect` | `0x80140390` | Expand named full-width backgrounds, not every overlay |
| `TSunGlass::draw` | `0x8017d354` | Full-screen tint/darken coverage |
| `TAfterEffect::perform` | `0x8022d4f8` | Full-screen dash-blur coverage with preserved history |
| `TShimmer::perform` | `0x8019f83c` | Keep projected lookup consistent with widened camera |
| `TBathWaterManager::draw_mist` | `0x801aa6cc` | Keep EFB-pixel copy and redraw coordinates identical |
| `TMirrorCamera::perform` | `0x80193fbc` | Keep mirror render and lookup projection paired |
| `TEfbCtrlTex::perform` | `0x802f8bac` | Preserve texture-pixel coordinate ownership |

## 2D policy

Menus and authored 4:3 video remain centered at correct aspect. Full-screen fades, curtains, and
named backgrounds expand to the viewport. HUD corner elements preserve their authored size and move
to the corresponding wide edge; center elements remain centered. Classification uses recovered J2D
pane identity and title behavior, not screen pixels.

`J2DPicture::drawFullSet` supplies destination rectangles for HUD pictures. `J2DPane` identity uses
the pane fourCC at `+0x10`; bounds are at `+0x14..+0x20`. The announcement background is J2DWindow
pane `te_w`, while text panes are `tet1` and `tet2`. These are retained layout facts for a future
typed native UI adapter.

## Screen-projected effects

The main camera and `SMS_GetLightPerspectiveForEffectMtx` must derive from one widened aspect. A
late projection-only squeeze makes the effect lookup disagree with rasterized geometry and causes
heat-haze/reflection displacement. Mirror and EFB-to-texture passes require their own paired
render/lookup or pixel-space policy; they cannot inherit a blanket exception.

## Verification

- Compare 4:3 and wide projections from identical source vertices.
- Prove additional horizontal scene coverage rather than a stretched final image.
- Exercise fade, menu, HUD, heat haze, water, mirror, and EFB-copy scenes separately.
- Plant a classification or projection perturbation that the capture must detect.
- Verify horizontal culling through the same aspect owner.
- Qualify x86_64, Apple Silicon macOS AArch64, and Android arm64-v8a independently.

No widescreen behavior is currently attached to gameplay; implementation starts after the
`gcnport` JIT product reaches the relevant title boundaries.
