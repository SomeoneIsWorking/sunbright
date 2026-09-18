#include "semantic_gpu_test_support.h"

#include <sunbright/native_render/sdl_gpu_frame_target.h>
#include <sunbright/native_render/semantic_3d_pass.h>

#include <SDL3/SDL.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>

namespace {

using sb::native_render::Color;
using sb::native_render::DecodedImageMipLevel;
using sb::native_render::DecodedImageView;
using sb::native_render::MeshResourceView;
using sb::native_render::MeshVertex;
using sb::native_render::ModelDraw;
using sb::native_render::PictureTexture;
using sb::native_render::SdlGpuFrameTarget;
using sb::native_render::SdlGpuPlatform;
using sb::native_render::Semantic3dPass;
using sb::native_render::SemanticFrame;
using sb::native_render::SemanticFramePixels;
using sb::native_render::test::encode_3d_and_readback;
using sb::native_render::test::hash;
using sb::native_render::test::linear_to_srgb;
using sb::native_render::test::near;
using sb::native_render::test::pixel;
using sb::native_render::test::require_color;
using sb::native_render::test::srgb_to_linear;

} // namespace

// Every model material family drawn on a real device and read back. This submits GPU work and is
// deliberately outside unguarded ctest: run it through tools/render/gpu_watch.py so a driver fault
// is captured and the exact process group stops.
int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SKIP: SDL video unavailable: " << SDL_GetError() << '\n';
        return 77;
    }
    SDL_Window* window =
        SDL_CreateWindow("Sunbright semantic 3D GPU control", 64, 64, SDL_WINDOW_HIDDEN);
    SdlGpuPlatform platform;
    std::string platformError;
    if (window == nullptr || !platform.initialize(window, {}, platformError)) {
        std::cerr << "SKIP: SDL GPU platform unavailable: "
                  << (platformError.empty() ? SDL_GetError() : platformError) << '\n';
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        return 77;
    }
    SDL_GPUDevice* device = platform.device();

    SdlGpuFrameTarget modelTarget;
    assert(modelTarget.initialize(platform, {.width = 16, .height = 16, .hasDepth = true},
                                  platformError));
    {
        Semantic3dPass pass(device);
        std::string error;
        const std::array<MeshVertex, 3> vertices{
            MeshVertex{.position = {-0.75F, -0.75F, 0.5F},
                       .uv = {0.25F, 0},
                       .uv1 = {0.75F, 0},
                       .matrixIndex = 1},
            MeshVertex{.position = {0.0F, 0.75F, 0.5F}, .uv = {0.25F, 0}, .uv1 = {0.75F, 0}},
            MeshVertex{.position = {0.75F, -0.75F, 0.5F}, .uv = {0.25F, 0}, .uv1 = {0.75F, 0}},
        };
        const MeshResourceView mesh{201, 1, vertices};
        ModelDraw model{
            .instance = 202,
            .mesh = {.resource = 201, .revision = 1, .vertexCount = 3},
            .pose = {.modelViews = {sb::native_render::Matrix3x4{
                                        .value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
                                    sb::native_render::Matrix3x4{
                                        .value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}},
                     .count = 2},
            .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
            .material = sb::native_render::UnlitColorMaterial{.baseColor = {1, 0, 0, 1}},
        };
        const auto render = [&](std::span<const ModelDraw> models,
                                std::span<const DecodedImageView> images = {}) {
            const SemanticFrame modelFrame{.targetWidth = 16,
                                           .targetHeight = 16,
                                           .models = models,
                                           .meshes = std::span<const MeshResourceView>(&mesh, 1),
                                           .images = images};
            SemanticFramePixels result{};
            if (!encode_3d_and_readback(pass, modelFrame, modelTarget, result, error) ||
                !error.empty()) {
                std::cerr << "semantic 3D control failed: " << error << '\n';
                std::abort();
            }
            return result;
        };

        // Culling controls: this triangle follows J3D's authored front-face winding. Back
        // culling keeps it, front culling removes it, and cull-all is an exact no-fragment draw.
        auto& material = std::get<sb::native_render::UnlitColorMaterial>(model.material);
        material.raster.cull = sb::native_render::ModelCullMode::Back;
        const SemanticFramePixels backCull = render(std::span<const ModelDraw>(&model, 1));
        assert(pixel(backCull, 8, 8).r > 0.9F);

        // Linear fog is evaluated from view-space depth in every model fragment shader. The
        // triangle sits at eye depth -0.5, exactly halfway through [-1, 0], so red material under
        // blue fog must become purple. Disabling fog is the adjacent control above.
        ModelDraw fogged = model;
        fogged.fog = {.mode = sb::native_render::ModelFogMode::Linear,
                      .start = -1.0F,
                      .end = 0.0F,
                      .color = {0, 0, 1, 1}};
        const SemanticFramePixels foggedFrame = render(std::span<const ModelDraw>(&fogged, 1));
        const Color foggedPixel = pixel(foggedFrame, 8, 8);
        const float halfLinearAsSrgb = linear_to_srgb(0.5F);
        const bool fogAnswer = near(foggedPixel.r, halfLinearAsSrgb) && foggedPixel.g < 0.01F &&
                               near(foggedPixel.b, halfLinearAsSrgb);
        if (!fogAnswer) {
            std::cerr << "linear fog control: got " << foggedPixel.r << ',' << foggedPixel.g << ','
                      << foggedPixel.b << " expected " << halfLinearAsSrgb << ",0,"
                      << halfLinearAsSrgb << '\n';
        }
        assert(fogAnswer);
        assert(hash(foggedFrame) != hash(backCull));

        material.raster.cull = sb::native_render::ModelCullMode::Front;
        const SemanticFramePixels frontCull = render(std::span<const ModelDraw>(&model, 1));
        require_color(pixel(frontCull, 8, 8), {});
        material.raster.cull = sb::native_render::ModelCullMode::All;
        const SemanticFramePixels allCull = render(std::span<const ModelDraw>(&model, 1));
        assert(hash(allCull) == hash(frontCull));

        // The first vertex selects the second pose matrix. Moving only that matrix must deform the
        // triangle through the shipping vertex upload; an ignored matrix index would hash exactly
        // like the rigid control.
        ModelDraw deformed = model;
        deformed.pose.modelViews[1].value[3] = 0.5F;
        std::get<sb::native_render::UnlitColorMaterial>(deformed.material).raster.cull =
            sb::native_render::ModelCullMode::Back;
        const SemanticFramePixels deformedFrame = render(std::span<const ModelDraw>(&deformed, 1));
        assert(hash(deformedFrame) != hash(backCull));

        // A texture-free diffuse material must use the colour shader with its computed lit vertex
        // colour. Green ambient light is a known-positive answer distinct from the red unlit
        // control above and from the black clear, so a missing material route cannot pass silently.
        ModelDraw litColor = model;
        litColor.instance = 220;
        litColor.material = sb::native_render::LitColorMaterial{
            .ambientColor = {0, 1, 0, 1},
            .usesVertexRgb = true,
            .usesVertexAlpha = true,
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const SemanticFramePixels litColorFrame = render(std::span<const ModelDraw>(&litColor, 1));
        require_color(pixel(litColorFrame, 8, 8), {0, 1, 0, 1});
        assert(hash(litColorFrame) != hash(backCull));

        // Texture-free specular control: green vertex diffuse is halved while a red directional
        // highlight contributes another half. Disabling only the highlight must remove red without
        // disturbing green, through the shipping vertex transform and colour fragment shader.
        std::array<MeshVertex, 3> colorSpecularVertices = vertices;
        for (MeshVertex& colorSpecularVertex : colorSpecularVertices) {
            colorSpecularVertex.color = {0, 1, 0, 1};
            colorSpecularVertex.normal = {0, 0, 1};
        }
        const MeshResourceView colorSpecularMesh{224, 1, colorSpecularVertices};
        ModelDraw colorSpecular = model;
        colorSpecular.instance = 225;
        colorSpecular.mesh = {.resource = 224, .revision = 1, .vertexCount = 3};
        colorSpecular.material = sb::native_render::LitSpecularColorMaterial{
            .baseColor = {1, 1, 1, 1},
            .ambientColor = {1, 1, 1, 1},
            .diffuseScale = {0.5F, 0.5F, 0.5F, 1},
            .specularScale = 2,
            .lighting = {.specular = {.directionToLight = {0, 0, 1},
                                      .color = {0.25F, 0, 0, 1},
                                      .shininess = 50}},
            .usesVertexRgb = true,
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const auto renderColorSpecular = [&] {
            const SemanticFrame modelFrame{
                .targetWidth = 16,
                .targetHeight = 16,
                .models = std::span<const ModelDraw>(&colorSpecular, 1),
                .meshes = std::span<const MeshResourceView>(&colorSpecularMesh, 1),
            };
            SemanticFramePixels result{};
            assert(encode_3d_and_readback(pass, modelFrame, modelTarget, result, error) &&
                   error.empty());
            return result;
        };
        const SemanticFramePixels colorSpecularHighlight = renderColorSpecular();
        const Color colorSpecularPixel = pixel(colorSpecularHighlight, 8, 8);
        assert(colorSpecularPixel.r > 0.70F && colorSpecularPixel.g > 0.70F &&
               colorSpecularPixel.b < 0.01F);
        std::get<sb::native_render::LitSpecularColorMaterial>(colorSpecular.material)
            .specularScale = 0;
        const SemanticFramePixels colorSpecularNoHighlight = renderColorSpecular();
        const Color colorSpecularNoHighlightPixel = pixel(colorSpecularNoHighlight, 8, 8);
        assert(colorSpecularNoHighlightPixel.r < 0.01F &&
               near(colorSpecularNoHighlightPixel.g, colorSpecularPixel.g));
        assert(hash(colorSpecularHighlight) != hash(colorSpecularNoHighlight));

        // Cutout threshold controls: 127/255 is rejected and the adjacent authored value 128/255
        // is accepted. This catches a disabled test and an off-by-one threshold independently.
        material.raster.cull = sb::native_render::ModelCullMode::None;
        material.raster.alphaTest = sb::native_render::ModelAlphaTest::GreaterOrEqualHalf;
        material.baseColor.a = 127.0F / 255.0F;
        const SemanticFramePixels belowCutout = render(std::span<const ModelDraw>(&model, 1));
        require_color(pixel(belowCutout, 8, 8), {});
        material.baseColor.a = 128.0F / 255.0F;
        const SemanticFramePixels atCutout = render(std::span<const ModelDraw>(&model, 1));
        assert(pixel(atCutout, 8, 8).r > 0.9F);

        // The additive lens effect uses a strict byte-64 alpha comparison. Byte 64 must be
        // discarded while the adjacent byte 65 must pass, proving the shader keeps the authored
        // strictness instead of silently turning it into a >= threshold.
        material.raster.alphaTest = sb::native_render::ModelAlphaTest::GreaterThan64;
        material.baseColor.a = 64.0F / 255.0F;
        const SemanticFramePixels effectThresholdBelow =
            render(std::span<const ModelDraw>(&model, 1));
        require_color(pixel(effectThresholdBelow, 8, 8), {});
        material.baseColor.a = 65.0F / 255.0F;
        const SemanticFramePixels effectThresholdAt = render(std::span<const ModelDraw>(&model, 1));
        assert(pixel(effectThresholdAt, 8, 8).r > 0.9F);

        // The texture fragment shader has its own alpha-rejection code, so exercise the adjacent
        // 127/128 controls there as well instead of inferring coverage from the colour shader.
        std::array<std::uint8_t, 4> cutoutTexel{255, 0, 0, 127};
        DecodedImageView cutoutImage{
            .resource = 204, .revision = 1, .width = 1, .height = 1, .rgba8 = cutoutTexel};
        ModelDraw texturedCutout = model;
        texturedCutout.instance = 205;
        texturedCutout.material = sb::native_render::UnlitTexturedMaterial{
            .texture = {.resource = 204, .revision = 1, .width = 1, .height = 1, .hasAlpha = true},
            .usesVertexColor = false,
            .raster = {.cull = sb::native_render::ModelCullMode::None,
                       .alphaTest = sb::native_render::ModelAlphaTest::GreaterOrEqualHalf}};
        const SemanticFramePixels textureBelow =
            render(std::span<const ModelDraw>(&texturedCutout, 1),
                   std::span<const DecodedImageView>(&cutoutImage, 1));
        require_color(pixel(textureBelow, 8, 8), {});
        cutoutTexel[3] = 128;
        cutoutImage.revision = 2;
        std::get<sb::native_render::UnlitTexturedMaterial>(texturedCutout.material)
            .texture.revision = 2;
        const SemanticFramePixels textureAt =
            render(std::span<const ModelDraw>(&texturedCutout, 1),
                   std::span<const DecodedImageView>(&cutoutImage, 1));
        assert(pixel(textureAt, 8, 8).r > 0.9F);

        // The shipping upload path must select the material's UV set, not merely carry both sets.
        // Primary UVs point at red and secondary UVs at green in the same two-texel image.
        const std::array<std::uint8_t, 8> uvChoiceTexels{255, 0, 0, 255, 0, 255, 0, 255};
        const DecodedImageView uvChoiceImage{
            .resource = 218, .revision = 1, .width = 2, .height = 1, .rgba8 = uvChoiceTexels};
        ModelDraw uvChoice = model;
        uvChoice.instance = 219;
        uvChoice.material = sb::native_render::UnlitTexturedMaterial{
            .texture = {.resource = 218, .revision = 1, .width = 2, .height = 1},
            .usesVertexColor = false,
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const SemanticFramePixels primaryUv =
            render(std::span<const ModelDraw>(&uvChoice, 1),
                   std::span<const DecodedImageView>(&uvChoiceImage, 1));
        require_color(pixel(primaryUv, 8, 8), {1, 0, 0, 1});
        std::get<sb::native_render::UnlitTexturedMaterial>(uvChoice.material).textureCoordinates =
            sb::native_render::ModelTextureCoordinates::Secondary;
        const SemanticFramePixels secondaryUv =
            render(std::span<const ModelDraw>(&uvChoice, 1),
                   std::span<const DecodedImageView>(&uvChoiceImage, 1));
        require_color(pixel(secondaryUv, 8, 8), {0, 1, 0, 1});
        assert(hash(primaryUv) != hash(secondaryUv));

        // The solid-colour mask material ignores texture RGB and amplifies only texture alpha.
        // Adjacent 31/32 texels prove its authored 4x alpha scale reaches the shipping fragment
        // shader: 31*4 stays below the 128 cutout, while 32*4 is accepted as solid green.
        std::array<std::uint8_t, 4> maskTexel{255, 0, 255, 31};
        DecodedImageView maskImage{
            .resource = 208, .revision = 1, .width = 1, .height = 1, .rgba8 = maskTexel};
        ModelDraw alphaMask = model;
        alphaMask.instance = 209;
        alphaMask.material = sb::native_render::AlphaMaskedColorMaterial{
            .texture = {.resource = 208, .revision = 1, .width = 1, .height = 1, .hasAlpha = true},
            .color = {0, 1, 0, 1},
            .alphaScale = 4,
            .raster = {.cull = sb::native_render::ModelCullMode::None,
                       .alphaTest = sb::native_render::ModelAlphaTest::GreaterOrEqualHalf},
        };
        const SemanticFramePixels maskBelow =
            render(std::span<const ModelDraw>(&alphaMask, 1),
                   std::span<const DecodedImageView>(&maskImage, 1));
        require_color(pixel(maskBelow, 8, 8), {});
        maskTexel[3] = 32;
        maskImage.revision = 2;
        std::get<sb::native_render::AlphaMaskedColorMaterial>(alphaMask.material).texture.revision =
            2;
        const SemanticFramePixels maskAt = render(std::span<const ModelDraw>(&alphaMask, 1),
                                                  std::span<const DecodedImageView>(&maskImage, 1));
        assert(pixel(maskAt, 8, 8).g > 0.9F);
        assert(pixel(maskAt, 8, 8).r < 0.01F);

        // The two-texture material must sample colour from UV0 and alpha from UV1. The mask's
        // magenta RGB is deliberately wrong and must be ignored. A 31-alpha mask is rejected after
        // the authored 4x scale, while the adjacent 32-alpha texel selected only by UV1 is
        // accepted.
        const std::array<std::uint8_t, 4> litColorTexel{0, 255, 0, 255};
        const std::array<std::uint8_t, 8> litMaskTexels{255, 0, 255, 31, 255, 0, 255, 32};
        const DecodedImageView litColorImage{
            .resource = 210, .revision = 1, .width = 1, .height = 1, .rgba8 = litColorTexel};
        const DecodedImageView litMaskImage{
            .resource = 211, .revision = 1, .width = 2, .height = 1, .rgba8 = litMaskTexels};
        std::array<MeshVertex, 3> litMaskVertices = vertices;
        for (MeshVertex& litMaskVertex : litMaskVertices)
            litMaskVertex.uv1 = {0.25F, 0.5F};
        MeshResourceView litMaskMesh{212, 1, litMaskVertices};
        ModelDraw litAlphaMask = model;
        litAlphaMask.instance = 213;
        litAlphaMask.mesh = {.resource = 212, .revision = 1, .vertexCount = 3};
        litAlphaMask.material = sb::native_render::LitTexturedAlphaMaskMaterial{
            .colorTexture = {.resource = 210, .revision = 1, .width = 1, .height = 1},
            .alphaMaskTexture = {.resource = 211,
                                 .revision = 1,
                                 .width = 2,
                                 .height = 1,
                                 .minFilter = sb::native_render::FilterMode::Nearest,
                                 .magFilter = sb::native_render::FilterMode::Nearest},
            .baseColor = {1, 1, 1, 1},
            .ambientColor = {1, 1, 1, 1},
            .lighting = {.pointLights = {{{.position = {0, 0, 1}}}}, .pointLightCount = 1},
            .alphaScale = 4,
            .raster = {.cull = sb::native_render::ModelCullMode::None,
                       .alphaTest = sb::native_render::ModelAlphaTest::GreaterOrEqualHalf},
        };
        const std::array<DecodedImageView, 2> litMaskImages{litColorImage, litMaskImage};
        const auto renderLitMask = [&] {
            const SemanticFrame modelFrame{
                .targetWidth = 16,
                .targetHeight = 16,
                .models = std::span<const ModelDraw>(&litAlphaMask, 1),
                .meshes = std::span<const MeshResourceView>(&litMaskMesh, 1),
                .images = litMaskImages,
            };
            SemanticFramePixels result{};
            assert(encode_3d_and_readback(pass, modelFrame, modelTarget, result, error) &&
                   error.empty());
            return result;
        };
        const SemanticFramePixels litMaskBelow = renderLitMask();
        require_color(pixel(litMaskBelow, 8, 8), {});
        for (MeshVertex& litMaskVertex : litMaskVertices)
            litMaskVertex.uv1 = {0.75F, 0.5F};
        litMaskMesh.revision = 2;
        litAlphaMask.mesh.revision = 2;
        const SemanticFramePixels litMaskAt = renderLitMask();
        assert(pixel(litMaskAt, 8, 8).g > 0.9F);
        assert(pixel(litMaskAt, 8, 8).r < 0.01F && pixel(litMaskAt, 8, 8).b < 0.01F);

        // Layered-material control: a white base texture multiplies a 3/8 red detail plus 5/8
        // green lit colour. Changing only the independently sampled detail to blue must move that
        // weighted contribution without disturbing green. This exercises the shipping two-image
        // shader and both UV-bearing vertex inputs.
        const std::array<std::uint8_t, 4> layeredBaseTexel{255, 255, 255, 255};
        std::array<std::uint8_t, 4> layeredDetailTexel{255, 0, 0, 255};
        std::array<DecodedImageView, 2> layeredImages{
            DecodedImageView{
                .resource = 221, .revision = 1, .width = 1, .height = 1, .rgba8 = layeredBaseTexel},
            DecodedImageView{.resource = 222,
                             .revision = 1,
                             .width = 1,
                             .height = 1,
                             .rgba8 = layeredDetailTexel},
        };
        ModelDraw layeredModel = model;
        layeredModel.instance = 223;
        layeredModel.material = sb::native_render::LitLayeredTexturedMaterial{
            .baseTexture = {.resource = 221, .revision = 1, .width = 1, .height = 1},
            .detailTexture = {.resource = 222, .revision = 1, .width = 1, .height = 1},
            .baseColor = {0, 1, 0, 1},
            .ambientColor = {1, 1, 1, 1},
            .detailWeight = 3.0F / 8.0F,
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const SemanticFramePixels redDetail =
            render(std::span<const ModelDraw>(&layeredModel, 1), layeredImages);
        const Color redDetailPixel = pixel(redDetail, 8, 8);
        assert(redDetailPixel.r > 0.60F && redDetailPixel.g > 0.75F && redDetailPixel.b < 0.01F);
        layeredDetailTexel = {0, 0, 255, 255};
        layeredImages[1].revision = 2;
        std::get<sb::native_render::LitLayeredTexturedMaterial>(layeredModel.material)
            .detailTexture.revision = 2;
        const SemanticFramePixels blueDetail =
            render(std::span<const ModelDraw>(&layeredModel, 1), layeredImages);
        const Color blueDetailPixel = pixel(blueDetail, 8, 8);
        assert(blueDetailPixel.r < 0.01F && near(blueDetailPixel.g, redDetailPixel.g));
        assert(blueDetailPixel.b > 0.60F);
        assert(hash(redDetail) != hash(blueDetail));

        // Tinted layered control: with a white base image, neutral 1/2 effect colour, no diffuse
        // source, and a 1/2 outer mix, changing only the independently sampled 3/8 detail image
        // must move the matching output channel. This exercises the dedicated shipping shader
        // without routing through the ordinary layered shader or the compatibility renderer.
        const std::array<std::uint8_t, 4> tintedBaseTexel{255, 255, 255, 255};
        std::array<std::uint8_t, 4> tintedDetailTexel{255, 0, 0, 255};
        std::array<DecodedImageView, 2> tintedImages{
            DecodedImageView{
                .resource = 224, .revision = 1, .width = 1, .height = 1, .rgba8 = tintedBaseTexel},
            DecodedImageView{.resource = 225,
                             .revision = 1,
                             .width = 1,
                             .height = 1,
                             .rgba8 = tintedDetailTexel},
        };
        ModelDraw tintedModel = model;
        tintedModel.instance = 226;
        tintedModel.material = sb::native_render::LitTintedLayeredSpecularMaterial{
            .baseTexture = {.resource = 224, .revision = 1, .width = 1, .height = 1},
            .detailTexture = {.resource = 225, .revision = 1, .width = 1, .height = 1},
            .baseColor = {0, 0, 0, 1},
            .effectColor = {0.5F, 0.5F, 0.5F, 1},
            .lighting = {.pointLights = {{{.position = {0, 0, 1}}, {.position = {0, 0, 1}}}},
                         .pointLightCount = 2,
                         .specular = {.color = {0, 0, 0, 1}}},
            .detailWeight = 3.0F / 8.0F,
            .layerWeight = 0.5F,
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const SemanticFramePixels tintedRed =
            render(std::span<const ModelDraw>(&tintedModel, 1), tintedImages);
        const Color tintedRedPixel = pixel(tintedRed, 8, 8);
        tintedDetailTexel = {0, 0, 255, 255};
        tintedImages[1].revision = 2;
        std::get<sb::native_render::LitTintedLayeredSpecularMaterial>(tintedModel.material)
            .detailTexture.revision = 2;
        const SemanticFramePixels tintedBlue =
            render(std::span<const ModelDraw>(&tintedModel, 1), tintedImages);
        const Color tintedBluePixel = pixel(tintedBlue, 8, 8);
        const bool tintedAnswer = tintedRedPixel.r > tintedBluePixel.r + 0.08F &&
                                  tintedBluePixel.b > tintedRedPixel.b + 0.08F &&
                                  near(tintedRedPixel.g, tintedBluePixel.g);
        if (!tintedAnswer) {
            std::cerr << "tinted layered control: red-detail=" << tintedRedPixel.r << ','
                      << tintedRedPixel.g << ',' << tintedRedPixel.b
                      << " blue-detail=" << tintedBluePixel.r << ',' << tintedBluePixel.g << ','
                      << tintedBluePixel.b << '\n';
        }
        assert(tintedAnswer);
        assert(hash(tintedRed) != hash(tintedBlue));

        // Masked-toon control: the authored hand-mask alpha selects the primary or alternate
        // image at 8-bit precision. Changing only that alpha across the authored byte threshold
        // must move the
        // shipping four-image shader from red to blue while its black ramp and zero highlights
        // stay inert.
        std::array<std::uint8_t, 4> maskedPrimaryTexel{255, 0, 0, 64};
        std::array<std::uint8_t, 4> maskedMaskTexel{0, 0, 0, 132};
        const std::array<std::uint8_t, 4> maskedAlternateTexel{0, 0, 255, 255};
        const std::array<std::uint8_t, 4> maskedRampTexel{0, 0, 0, 255};
        std::array<DecodedImageView, 4> maskedImages{
            DecodedImageView{.resource = 227,
                             .revision = 1,
                             .width = 1,
                             .height = 1,
                             .rgba8 = maskedPrimaryTexel},
            DecodedImageView{
                .resource = 228, .revision = 1, .width = 1, .height = 1, .rgba8 = maskedMaskTexel},
            DecodedImageView{.resource = 229,
                             .revision = 1,
                             .width = 1,
                             .height = 1,
                             .rgba8 = maskedAlternateTexel},
            DecodedImageView{
                .resource = 230, .revision = 1, .width = 1, .height = 1, .rgba8 = maskedRampTexel},
        };
        ModelDraw maskedModel = model;
        maskedModel.instance = 231;
        maskedModel.material = sb::native_render::LitMaskedToonMaterial{
            .primaryTexture = {.resource = 227, .revision = 1, .width = 1, .height = 1},
            .maskTexture = {.resource = 228, .revision = 1, .width = 1, .height = 1},
            .alternateTexture = {.resource = 229, .revision = 1, .width = 1, .height = 1},
            .lightRampTexture = {.resource = 230, .revision = 1, .width = 1, .height = 1},
            .baseColor = {0, 0, 0, 1},
            .lighting = {.pointLights = {{{.position = {0, 0, 1}}}},
                         .pointLightCount = 1,
                         .specular = {.directionToLight = {0, 0, 1}, .color = {0, 0, 0, 1}}},
            .lightRampWeight = 3.0F / 8.0F,
            .maskThreshold = 131.0F / 255.0F,
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const SemanticFramePixels maskedPrimary =
            render(std::span<const ModelDraw>(&maskedModel, 1), maskedImages);
        const Color maskedPrimaryPixel = pixel(maskedPrimary, 8, 8);
        assert(maskedPrimaryPixel.r > 0.9F && maskedPrimaryPixel.b < 0.01F);
        maskedMaskTexel[3] = 131;
        maskedImages[1].revision = 2;
        std::get<sb::native_render::LitMaskedToonMaterial>(maskedModel.material)
            .maskTexture.revision = 2;
        const SemanticFramePixels maskedAlternate =
            render(std::span<const ModelDraw>(&maskedModel, 1), maskedImages);
        const Color maskedAlternatePixel = pixel(maskedAlternate, 8, 8);
        assert(maskedAlternatePixel.r < 0.01F && maskedAlternatePixel.b > 0.9F);
        assert(hash(maskedPrimary) != hash(maskedAlternate));

        maskedMaskTexel[3] = 132;
        maskedImages[1].revision = 3;
        auto& maskedMaterial =
            std::get<sb::native_render::LitMaskedToonMaterial>(maskedModel.material);
        maskedMaterial.maskTexture.revision = 3;
        maskedMaterial.alphaSource = sb::native_render::ModelAlphaSource::PrimaryTexture;
        const SemanticFramePixels maskedTextureAlpha =
            render(std::span<const ModelDraw>(&maskedModel, 1), maskedImages);
        const Color maskedTextureAlphaPixel = pixel(maskedTextureAlpha, 8, 8);
        assert(maskedTextureAlphaPixel.r > 0.9F && maskedTextureAlphaPixel.b < 0.01F);
        assert(near(maskedTextureAlphaPixel.a, 64.0F / 255.0F));

        // Affine texture control for the specular-material shader path. The baseline exercises
        // texture * diffuse colour; changing only the semantic tint must add red while preserving
        // green. This runs the shipping vertex upload and fragment shader, not a CPU copy.
        const std::array<std::uint8_t, 4> affineTexel{128, 64, 32, 255};
        const DecodedImageView affineImage{
            .resource = 206, .revision = 1, .width = 1, .height = 1, .rgba8 = affineTexel};
        ModelDraw affineModel = model;
        affineModel.instance = 207;
        affineModel.material = sb::native_render::LitSpecularTexturedMaterial{
            .texture = {.resource = 206, .revision = 1, .width = 1, .height = 1},
            .baseColor = {0.5F, 0.5F, 0.5F, 1},
            .ambientColor = {1, 1, 1, 1},
            .lighting = {.specular = {.directionToLight = {0, 0, 1},
                                      .color = {0, 0, 0, 1},
                                      .shininess = 1}},
        };
        const SemanticFramePixels affineBaseline =
            render(std::span<const ModelDraw>(&affineModel, 1),
                   std::span<const DecodedImageView>(&affineImage, 1));
        auto& affineMaterial =
            std::get<sb::native_render::LitSpecularTexturedMaterial>(affineModel.material);
        affineMaterial.textureDiffuseScale = {0.75F, 1, 1, 1};
        affineMaterial.additiveColor = {0.5F, 0, 0, 0};
        affineMaterial.specularScale = 2.0F;
        const SemanticFramePixels affineTinted =
            render(std::span<const ModelDraw>(&affineModel, 1),
                   std::span<const DecodedImageView>(&affineImage, 1));
        const Color baselinePixel = pixel(affineBaseline, 8, 8);
        const Color tintedPixel = pixel(affineTinted, 8, 8);
        // Both the sampled image and target are sRGB. Check the shipping conversion around the
        // linear affine operation instead of treating either byte value as linear light.
        const float red = srgb_to_linear(128.0F / 255.0F);
        const float green = srgb_to_linear(64.0F / 255.0F);
        assert(near(baselinePixel.r, linear_to_srgb(red * 0.5F)));
        assert(near(tintedPixel.r, linear_to_srgb(red * 0.375F + 0.5F)));
        assert(near(tintedPixel.g, linear_to_srgb(green * 0.5F)));
        assert(near(tintedPixel.g, baselinePixel.g));
        assert(hash(affineBaseline) != hash(affineTinted));

        // Blend control: the same half-alpha red replaces black when opaque, but source-alpha
        // blending produces the distinct sRGB-encoded half-intensity result.
        material.raster.alphaTest = sb::native_render::ModelAlphaTest::PassAll;
        material.raster.blend = sb::native_render::ModelBlendMode::Replace;
        material.baseColor = {1, 0, 0, 0.5F};
        const SemanticFramePixels replaced = render(std::span<const ModelDraw>(&model, 1));
        material.raster.blend = sb::native_render::ModelBlendMode::SourceAlpha;
        const SemanticFramePixels blended = render(std::span<const ModelDraw>(&model, 1));
        assert(pixel(replaced, 8, 8).r > 0.95F);
        assert(pixel(blended, 8, 8).r > 0.65F && pixel(blended, 8, 8).r < 0.80F);
        assert(hash(replaced) != hash(blended));

        // Premultiplied-alpha blending keeps source RGB unscaled while applying the same
        // one-minus-source-alpha destination factor. Draw over blue so this cannot collapse to
        // the replace result: only red differs from straight-alpha blending, while blue matches.
        ModelDraw blueBackground = model;
        auto& blueMaterial =
            std::get<sb::native_render::UnlitColorMaterial>(blueBackground.material);
        blueMaterial.baseColor = {0, 0, 1, 1};
        blueMaterial.raster.blend = sb::native_render::ModelBlendMode::Replace;
        blueMaterial.raster.depthWrite = false;
        ModelDraw translucentRed = model;
        translucentRed.instance = 214;
        auto& translucentMaterial =
            std::get<sb::native_render::UnlitColorMaterial>(translucentRed.material);
        translucentMaterial.baseColor = {0.25F, 0, 0, 0.5F};
        translucentMaterial.raster.depthWrite = false;
        translucentMaterial.raster.blend = sb::native_render::ModelBlendMode::SourceAlpha;
        std::array<ModelDraw, 2> blendLayers{blueBackground, translucentRed};
        const SemanticFramePixels straightBlend = render(blendLayers);
        std::get<sb::native_render::UnlitColorMaterial>(blendLayers[1].material).raster.blend =
            sb::native_render::ModelBlendMode::PremultipliedAlpha;
        const SemanticFramePixels premultipliedBlend = render(blendLayers);
        const Color straightPixel = pixel(straightBlend, 8, 8);
        const Color premultipliedPixel = pixel(premultipliedBlend, 8, 8);
        assert(premultipliedPixel.r > straightPixel.r + 0.1F);
        assert(near(premultipliedPixel.b, straightPixel.b));
        assert(hash(premultipliedBlend) != hash(straightBlend));

        // Additive control: source-alpha plus destination-one keeps the existing blue glow
        // contribution, unlike ordinary alpha compositing's one-minus-source-alpha destination.
        std::get<sb::native_render::UnlitColorMaterial>(blendLayers[1].material).raster.blend =
            sb::native_render::ModelBlendMode::Additive;
        const SemanticFramePixels additiveBlend = render(blendLayers);
        const Color additivePixel = pixel(additiveBlend, 8, 8);
        assert(additivePixel.b > straightPixel.b + 0.25F);
        assert(additivePixel.r > straightPixel.r - 0.05F &&
               additivePixel.r < straightPixel.r + 0.05F);
        assert(hash(additiveBlend) != hash(straightBlend));

        // Inverse-source-colour control: the source is kept whole while the destination survives
        // by one minus that source. Over a mid-grey background a red source must leave red at
        // full, and grey's own half in the two channels the source does not occupy -- which
        // ordinary alpha compositing cannot produce.
        ModelDraw greyBackground = model;
        auto& greyMaterial =
            std::get<sb::native_render::UnlitColorMaterial>(greyBackground.material);
        greyMaterial.baseColor = {0.5F, 0.5F, 0.5F, 1};
        greyMaterial.raster.blend = sb::native_render::ModelBlendMode::Replace;
        greyMaterial.raster.depthWrite = false;
        ModelDraw inverseSource = model;
        inverseSource.instance = 240;
        auto& inverseMaterial =
            std::get<sb::native_render::UnlitColorMaterial>(inverseSource.material);
        inverseMaterial.baseColor = {1, 0, 0, 1};
        inverseMaterial.raster.depthWrite = false;
        inverseMaterial.raster.blend = sb::native_render::ModelBlendMode::InverseSourceColor;
        const std::array<ModelDraw, 2> inverseLayers{greyBackground, inverseSource};
        const SemanticFramePixels inverseBlend = render(inverseLayers);
        const Color inversePixel = pixel(inverseBlend, 8, 8);
        assert(inversePixel.r > 0.95F);
        assert(near(inversePixel.g, linear_to_srgb(0.5F)));
        assert(near(inversePixel.b, linear_to_srgb(0.5F)));

        // Doubled texture pair: two images multiplied together and doubled under one tint. A
        // mid-grey base against a white detail must come out at twice the base; darkening the
        // detail to the same grey must halve it again, which proves the second image reaches the
        // product rather than the first being doubled alone.
        const std::array<std::uint8_t, 4> whiteTexel{255, 255, 255, 255};
        std::array<std::uint8_t, 4> pairDetailTexel{255, 255, 255, 255};
        const std::array<std::uint8_t, 4> greyTexel{128, 128, 128, 255};
        std::array<DecodedImageView, 2> pairImages{
            DecodedImageView{
                .resource = 241, .revision = 1, .width = 1, .height = 1, .rgba8 = greyTexel},
            DecodedImageView{
                .resource = 242, .revision = 1, .width = 1, .height = 1, .rgba8 = pairDetailTexel},
        };
        ModelDraw pairModel = model;
        pairModel.instance = 243;
        pairModel.material = sb::native_render::DoubledTexturePairMaterial{
            .baseTexture = {.resource = 241, .revision = 1, .width = 1, .height = 1},
            .detailTexture = {.resource = 242, .revision = 1, .width = 1, .height = 1},
            .tint = {1, 1, 1, 1},
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const float greyLinear = srgb_to_linear(128.0F / 255.0F);
        const SemanticFramePixels pairWhiteDetail =
            render(std::span<const ModelDraw>(&pairModel, 1), pairImages);
        assert(near(pixel(pairWhiteDetail, 8, 8).r, linear_to_srgb(2.0F * greyLinear)));
        pairDetailTexel = greyTexel;
        pairImages[1].revision = 2;
        std::get<sb::native_render::DoubledTexturePairMaterial>(pairModel.material)
            .detailTexture.revision = 2;
        const SemanticFramePixels pairGreyDetail =
            render(std::span<const ModelDraw>(&pairModel, 1), pairImages);
        assert(near(pixel(pairGreyDetail, 8, 8).r, linear_to_srgb(2.0F * greyLinear * greyLinear)));
        assert(hash(pairGreyDetail) != hash(pairWhiteDetail));

        // The pair's two opacity answers are different materials, not a spare field: with both
        // images at half alpha the doubled product keeps that half, while the constant spelling
        // publishes the tint's own alpha and ignores the images entirely.
        const std::array<std::uint8_t, 4> halfAlphaTexel{255, 255, 255, 128};
        const std::array<DecodedImageView, 2> pairAlphaImages{
            DecodedImageView{
                .resource = 244, .revision = 1, .width = 1, .height = 1, .rgba8 = halfAlphaTexel},
            DecodedImageView{
                .resource = 245, .revision = 1, .width = 1, .height = 1, .rgba8 = halfAlphaTexel},
        };
        ModelDraw pairAlpha = model;
        pairAlpha.instance = 246;
        pairAlpha.material = sb::native_render::DoubledTexturePairMaterial{
            .baseTexture = {.resource = 244, .revision = 1, .width = 1, .height = 1},
            .detailTexture = {.resource = 245, .revision = 1, .width = 1, .height = 1},
            .tint = {1, 1, 1, 0.25F},
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const SemanticFramePixels pairProductAlpha =
            render(std::span<const ModelDraw>(&pairAlpha, 1), pairAlphaImages);
        const float halfAlpha = 128.0F / 255.0F;
        assert(near(pixel(pairProductAlpha, 8, 8).a, 0.25F * halfAlpha * halfAlpha * 2.0F));
        std::get<sb::native_render::DoubledTexturePairMaterial>(pairAlpha.material).alphaMode =
            sb::native_render::ModelPairAlphaMode::Constant;
        const SemanticFramePixels pairConstantAlpha =
            render(std::span<const ModelDraw>(&pairAlpha, 1), pairAlphaImages);
        assert(near(pixel(pairConstantAlpha, 8, 8).a, 0.25F));

        // Tinted texture sum: each image is scaled by its own tint and the second is added to the
        // first. Red and green tints over white images must sum to yellow; blanking the second
        // image must leave red alone, which no single-layer program would do.
        std::array<std::uint8_t, 4> sumSecondTexel{255, 255, 255, 255};
        std::array<DecodedImageView, 2> sumImages{
            DecodedImageView{
                .resource = 247, .revision = 1, .width = 1, .height = 1, .rgba8 = whiteTexel},
            DecodedImageView{
                .resource = 248, .revision = 1, .width = 1, .height = 1, .rgba8 = sumSecondTexel},
        };
        ModelDraw sumModel = model;
        sumModel.instance = 249;
        sumModel.material = sb::native_render::TintedTextureSumMaterial{
            .firstTexture = {.resource = 247, .revision = 1, .width = 1, .height = 1},
            .secondTexture = {.resource = 248, .revision = 1, .width = 1, .height = 1},
            .firstTint = {1, 0, 0, 1},
            .secondTint = {0, 1, 0, 0},
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const SemanticFramePixels summed =
            render(std::span<const ModelDraw>(&sumModel, 1), sumImages);
        const Color summedPixel = pixel(summed, 8, 8);
        assert(summedPixel.r > 0.95F && summedPixel.g > 0.95F && summedPixel.b < 0.01F);
        sumSecondTexel = {0, 0, 0, 255};
        sumImages[1].revision = 2;
        std::get<sb::native_render::TintedTextureSumMaterial>(sumModel.material)
            .secondTexture.revision = 2;
        const SemanticFramePixels summedWithoutSecond =
            render(std::span<const ModelDraw>(&sumModel, 1), sumImages);
        const Color withoutSecondPixel = pixel(summedWithoutSecond, 8, 8);
        assert(withoutSecondPixel.r > 0.95F && withoutSecondPixel.g < 0.01F);

        // Masked doubled texture: the first image's colour is authored to be discarded and only
        // its alpha survives. Turning that image blue must change nothing, while changing its
        // alpha must change the output -- the two halves of the claim, checked separately.
        std::array<std::uint8_t, 4> maskedPairOpacityTexel{255, 255, 255, 128};
        const std::array<std::uint8_t, 4> maskedPairColorTexel{128, 0, 0, 255};
        std::array<DecodedImageView, 2> maskedPairImages{
            DecodedImageView{.resource = 250,
                             .revision = 1,
                             .width = 1,
                             .height = 1,
                             .rgba8 = maskedPairOpacityTexel},
            DecodedImageView{.resource = 251,
                             .revision = 1,
                             .width = 1,
                             .height = 1,
                             .rgba8 = maskedPairColorTexel},
        };
        ModelDraw maskedPairModel = model;
        maskedPairModel.instance = 252;
        maskedPairModel.material = sb::native_render::MaskedDoubledTextureMaterial{
            .opacityTexture = {.resource = 250, .revision = 1, .width = 1, .height = 1},
            .colorTexture = {.resource = 251, .revision = 1, .width = 1, .height = 1},
            .tint = {1, 1, 1, 1},
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const SemanticFramePixels maskedPairBaseline =
            render(std::span<const ModelDraw>(&maskedPairModel, 1), maskedPairImages);
        const Color maskedPairPixel = pixel(maskedPairBaseline, 8, 8);
        assert(near(maskedPairPixel.r, linear_to_srgb(2.0F * greyLinear)));
        assert(near(maskedPairPixel.a, halfAlpha));
        maskedPairOpacityTexel = {0, 0, 255, 128};
        maskedPairImages[0].revision = 2;
        std::get<sb::native_render::MaskedDoubledTextureMaterial>(maskedPairModel.material)
            .opacityTexture.revision = 2;
        const SemanticFramePixels maskedPairRecoloured =
            render(std::span<const ModelDraw>(&maskedPairModel, 1), maskedPairImages);
        assert(hash(maskedPairRecoloured) == hash(maskedPairBaseline));
        maskedPairOpacityTexel = {0, 0, 255, 255};
        maskedPairImages[0].revision = 3;
        std::get<sb::native_render::MaskedDoubledTextureMaterial>(maskedPairModel.material)
            .opacityTexture.revision = 3;
        const SemanticFramePixels maskedPairOpaque =
            render(std::span<const ModelDraw>(&maskedPairModel, 1), maskedPairImages);
        assert(near(pixel(maskedPairOpaque, 8, 8).a, 1.0F));
        assert(hash(maskedPairOpaque) != hash(maskedPairBaseline));

        // Interpolated registers: one image chooses per channel between two authored registers,
        // and the stage biases by a half and halves the sum. A black chooser must give the lower
        // register's half-biased half, a white one the upper register's -- the two ends of the
        // interpolation, where the choice is exact regardless of colour space.
        std::array<std::uint8_t, 4> chooserTexel{0, 0, 0, 255};
        const std::array<std::uint8_t, 4> blackTexel{0, 0, 0, 255};
        std::array<DecodedImageView, 2> registerImages{
            DecodedImageView{
                .resource = 253, .revision = 1, .width = 1, .height = 1, .rgba8 = chooserTexel},
            DecodedImageView{
                .resource = 254, .revision = 1, .width = 1, .height = 1, .rgba8 = blackTexel},
        };
        ModelDraw registerModel = model;
        registerModel.instance = 255;
        registerModel.material = sb::native_render::InterpolatedRegisterMaterial{
            .blendTexture = {.resource = 253, .revision = 1, .width = 1, .height = 1},
            .detailTexture = {.resource = 254, .revision = 1, .width = 1, .height = 1},
            .lowerColor = {1, 0, 0, 1},
            .upperColor = {0, 0, 1, 0},
            .colorOffset = 0,
            .detailWeight = 5.0F / 8.0F,
            .alphaGain = 1.0F,
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const SemanticFramePixels lowerChosen =
            render(std::span<const ModelDraw>(&registerModel, 1), registerImages);
        const Color lowerPixel = pixel(lowerChosen, 8, 8);
        assert(near(lowerPixel.r, linear_to_srgb(0.75F)));
        assert(near(lowerPixel.g, linear_to_srgb(0.25F)));
        assert(near(lowerPixel.b, linear_to_srgb(0.25F)));
        chooserTexel = {255, 255, 255, 255};
        registerImages[0].revision = 2;
        std::get<sb::native_render::InterpolatedRegisterMaterial>(registerModel.material)
            .blendTexture.revision = 2;
        const SemanticFramePixels upperChosen =
            render(std::span<const ModelDraw>(&registerModel, 1), registerImages);
        const Color upperPixel = pixel(upperChosen, 8, 8);
        assert(near(upperPixel.r, linear_to_srgb(0.25F)));
        assert(near(upperPixel.b, linear_to_srgb(0.75F)));
        assert(hash(upperChosen) != hash(lowerChosen));

        // Depth-write control: a near red draw prevents a later far green draw only when the near
        // material writes depth. Both cases retain LEQUAL testing, isolating the write bit.
        ModelDraw near = model;
        auto& nearMaterial = std::get<sb::native_render::UnlitColorMaterial>(near.material);
        nearMaterial.baseColor = {1, 0, 0, 1};
        nearMaterial.raster.blend = sb::native_render::ModelBlendMode::Replace;
        nearMaterial.raster.depthWrite = true;
        ModelDraw far = near;
        far.instance = 203;
        far.pose.modelViews[0].value[11] = 0.5F;
        std::get<sb::native_render::UnlitColorMaterial>(far.material).baseColor = {0, 1, 0, 1};
        std::array<ModelDraw, 2> layered{near, far};
        const SemanticFramePixels depthWritten = render(layered);
        assert(pixel(depthWritten, 8, 8).r > 0.9F && pixel(depthWritten, 8, 8).g < 0.1F);
        std::get<sb::native_render::UnlitColorMaterial>(layered[0].material).raster.depthWrite =
            false;
        const SemanticFramePixels depthNotWritten = render(layered);
        assert(pixel(depthNotWritten, 8, 8).g > 0.9F && pixel(depthNotWritten, 8, 8).r < 0.1F);
    }
    modelTarget.shutdown();

    assert(platform.shutdown(platformError));
    SDL_DestroyWindow(window);
    SDL_Quit();
}
