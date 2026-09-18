#include "semantic_gpu_test_support.h"

#include <sunbright/native_render/sdl_gpu_frame_target.h>
#include <sunbright/native_render/sdl_semantic_frame_client.h>
#include <sunbright/native_render/semantic_2d_pass.h>
#include <sunbright/native_render/semantic_3d_pass.h>
#include <sunbright/native_render/semantic_frame_mode.h>
#include <sunbright/native_render/semantic_sink.h>

#include <SDL3/SDL.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

using sb::native_render::Color;
using sb::native_render::DecodedImageMipLevel;
using sb::native_render::DecodedImageView;
using sb::native_render::GlyphCommand;
using sb::native_render::GlyphDraw;
using sb::native_render::MeshResourceView;
using sb::native_render::MeshVertex;
using sb::native_render::ModelDraw;
using sb::native_render::PictureCommand;
using sb::native_render::PictureDraw;
using sb::native_render::PictureTexture;
using sb::native_render::SdlGpuFrameTarget;
using sb::native_render::SdlGpuPlatform;
using sb::native_render::Semantic2dPass;
using sb::native_render::Semantic3dPass;
using sb::native_render::SemanticDraw;
using sb::native_render::SemanticFrame;
using sb::native_render::SemanticFramePixels;
using sb::native_render::SolidRectangleDraw;
using sb::native_render::Vec2;
using sb::native_render::test::encode_3d_and_readback;
using sb::native_render::test::encode_and_readback;
using sb::native_render::test::hash;
using sb::native_render::test::near;
using sb::native_render::test::pixel;
using sb::native_render::test::require_color;

PictureCommand command() {
    PictureCommand picture{};
    picture.instance = 1;
    picture.positions = {Vec2{0, 0}, Vec2{16, 0}, Vec2{0, 16}, Vec2{16, 16}};
    picture.uv = {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}, Vec2{1, 1}};
    picture.clip = {.enabled = true, .x = 4, .y = 4, .width = 8, .height = 8};
    picture.material.textureCount = 1;
    picture.material.textures[0] =
        PictureTexture{.resource = 9, .width = 2, .height = 2, .hasAlpha = true};
    return picture;
}

SolidRectangleDraw solid(std::uint64_t instance, float left, float top, float right, float bottom,
                         Color color) {
    return {
        {.origin = {0, 0}, .extent = {16, 16}, .viewport = {0, 0, 16, 16}},
        {.instance = instance,
         .source = sb::native_render::SolidRectangleSource::Gc2dFillRect,
         .positions = {Vec2{left, top}, Vec2{right, top}, Vec2{left, bottom}, Vec2{right, bottom}},
         .corner = {color, color, color, color}}};
}

} // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SKIP: SDL video unavailable: " << SDL_GetError() << '\n';
        return 77;
    }
    SDL_Window* window =
        SDL_CreateWindow("Sunbright semantic GPU control", 64, 64, SDL_WINDOW_HIDDEN);
    SdlGpuPlatform platform;
    std::string platformError;
    if (window == nullptr || !platform.initialize(window, {}, platformError)) {
        std::cerr << "SKIP: SDL GPU platform unavailable: "
                  << (platformError.empty() ? SDL_GetError() : platformError) << '\n';
        if (window != nullptr)
            SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }
    SdlGpuFrameTarget target;
    assert(
        target.initialize(platform, {.width = 16, .height = 16, .hasDepth = false}, platformError));
    SDL_GPUDevice* device = platform.device();

    // 2x2 RGBA: red, green / blue, half-alpha white. Nearest sampling makes each quadrant an
    // unmistakable known-positive; the clip rectangle proves the production scissor conversion.
    std::array<std::uint8_t, 16> rgba{255, 0, 0,   255, 0,   255, 0,   255,
                                      0,   0, 255, 255, 255, 255, 255, 128};
    DecodedImageView image{.resource = 9, .width = 2, .height = 2, .rgba8 = rgba};
    PictureCommand picture = command();
    PictureDraw draw{{.origin = {0, 0}, .extent = {16, 16}, .viewport = {0, 0, 16, 16}}, picture};
    SemanticDraw semanticDraw{draw};
    const SemanticFrame frame{.targetWidth = 16,
                              .targetHeight = 16,
                              .draws = std::span<const SemanticDraw>(&semanticDraw, 1),
                              .images = std::span<const DecodedImageView>(&image, 1)};

    {
        // The renderer client must release its pipeline/shaders before the host destroys the one
        // shared device. Vulkan validation is enabled specifically to enforce that ownership.
        Semantic2dPass pass(device);
        std::string error;
        SemanticFramePixels first{};
        assert(encode_and_readback(pass, frame, target, first, error) && error.empty());
        assert(pass.resident_image_count() == 1);
        require_color(pixel(first, 1, 1), {});
        require_color(pixel(first, 5, 5), {1, 0, 0, 1});
        require_color(pixel(first, 10, 5), {0, 1, 0, 1});
        require_color(pixel(first, 5, 10), {0, 0, 1, 1});
        const Color alphaPixel = pixel(first, 10, 10);
        assert(alphaPixel.r > 0.70f && alphaPixel.g > 0.70f && alphaPixel.b > 0.70f);
        assert(near(alphaPixel.a, 128.0f / 255.0f));

        // Repeated resolutions in one frame reuse the same resident image. The opaque texel
        // remains red even though the same resource is drawn twice; the next frame also reuses it.
        const std::array<SemanticDraw, 2> repeatedDraws{semanticDraw, semanticDraw};
        SemanticFrame repeatedFrame = frame;
        repeatedFrame.draws = repeatedDraws;
        SemanticFramePixels repeatedResolution{};
        assert(encode_and_readback(pass, repeatedFrame, target, repeatedResolution, error) &&
               error.empty());
        assert(pass.resident_image_count() == 1);
        require_color(pixel(repeatedResolution, 5, 5), {1, 0, 0, 1});
        assert(encode_and_readback(pass, frame, target, repeatedResolution, error) &&
               error.empty());
        assert(pass.resident_image_count() == 1 && hash(repeatedResolution) == hash(first));

        // Known-positive mip control: this one-pixel draw minifies a 4x4 red base level. The
        // only authored lower level is blue, so the sampled pixel proves that the production image
        // cache uploaded the lower level and exposed it to the semantic sampler.
        std::array<std::uint8_t, 64> redBase{};
        for (std::size_t offset = 0; offset < redBase.size(); offset += 4) {
            redBase[offset] = 255;
            redBase[offset + 3] = 255;
        }
        DecodedImageMipLevel blueMip{
            .width = 2,
            .height = 2,
            .rgba8 = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255}};
        const DecodedImageView mipmappedImage{.resource = 10,
                                              .revision = 1,
                                              .width = 4,
                                              .height = 4,
                                              .rgba8 = redBase,
                                              .mipLevels = std::span(&blueMip, 1)};
        PictureCommand minified = command();
        minified.instance = 10;
        minified.positions = {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}, Vec2{1, 1}};
        minified.clip = {};
        minified.material.textures[0] = {.resource = 10,
                                         .revision = 1,
                                         .width = 4,
                                         .height = 4,
                                         .minFilter = sb::native_render::FilterMode::Nearest,
                                         .magFilter = sb::native_render::FilterMode::Nearest,
                                         .mipFilter = sb::native_render::MipFilter::Nearest};
        const SemanticDraw minifiedDraw{PictureDraw{draw.canvas, minified}};
        const SemanticFrame mipmappedFrame{
            .targetWidth = 16,
            .targetHeight = 16,
            .draws = std::span<const SemanticDraw>(&minifiedDraw, 1),
            .images = std::span<const DecodedImageView>(&mipmappedImage, 1)};
        SemanticFramePixels mipmappedResult{};
        assert(encode_and_readback(pass, mipmappedFrame, target, mipmappedResult, error) &&
               error.empty());
        require_color(pixel(mipmappedResult, 0, 0), {0, 0, 1, 1});

        // The same full image chain must not change a material that selects no mip filtering.
        // This distinguishes a correctly uploaded chain from a cache that silently enables it for
        // every sampler sharing the image.
        PictureCommand noMip = minified;
        noMip.instance = 12;
        noMip.material.textures[0].mipFilter = sb::native_render::MipFilter::None;
        const SemanticDraw noMipDraw{PictureDraw{draw.canvas, noMip}};
        const SemanticFrame noMipFrame{.targetWidth = 16,
                                       .targetHeight = 16,
                                       .draws = std::span<const SemanticDraw>(&noMipDraw, 1),
                                       .images =
                                           std::span<const DecodedImageView>(&mipmappedImage, 1)};
        SemanticFramePixels noMipResult{};
        assert(encode_and_readback(pass, noMipFrame, target, noMipResult, error) && error.empty());
        require_color(pixel(noMipResult, 0, 0), {1, 0, 0, 1});

        // Existing one-level images remain valid when their material requests mip filtering. The
        // sampler clamps to their only authored level instead of making unrelated UI resources an
        // error while the image adapter has no lower-resolution source data to provide.
        const DecodedImageView baseOnlyImage{
            .resource = 11, .revision = 1, .width = 4, .height = 4, .rgba8 = redBase};
        PictureCommand baseOnly = minified;
        baseOnly.instance = 11;
        baseOnly.material.textures[0].resource = 11;
        const SemanticDraw baseOnlyDraw{PictureDraw{draw.canvas, baseOnly}};
        const SemanticFrame baseOnlyFrame{.targetWidth = 16,
                                          .targetHeight = 16,
                                          .draws = std::span<const SemanticDraw>(&baseOnlyDraw, 1),
                                          .images =
                                              std::span<const DecodedImageView>(&baseOnlyImage, 1)};
        SemanticFramePixels baseOnlyResult{};
        assert(encode_and_readback(pass, baseOnlyFrame, target, baseOnlyResult, error) &&
               error.empty());
        require_color(pixel(baseOnlyResult, 0, 0), {1, 0, 0, 1});

        SemanticFramePixels repeated{};
        assert(pass.render_and_readback(frame, repeated, error) && error.empty());
        assert(hash(repeated) == hash(first));

        // No-op control: a valid draw wholly outside its semantic clip must not perturb the target
        // or be misclassified as a malformed command.
        PictureCommand clipped = picture;
        clipped.instance = 2;
        clipped.clip = {.enabled = true, .x = 32, .y = 32, .width = 4, .height = 4};
        const std::array<SemanticDraw, 2> clippedDraws{draw, PictureDraw{draw.canvas, clipped}};
        SemanticFrame clippedFrame = frame;
        clippedFrame.draws = clippedDraws;
        SemanticFramePixels clippedResult{};
        assert(pass.render_and_readback(clippedFrame, clippedResult, error) && error.empty());
        assert(hash(clippedResult) == hash(first));

        // Known-different per-draw canvas: the same logical picture is mapped into a centered
        // physical sub-viewport. This catches a backend that incorrectly treats canvas as one
        // frame-wide value or drops the viewport origin.
        PictureDraw inset = draw;
        inset.canvas.viewport = {4, 4, 8, 8};
        const SemanticDraw insetDraw{inset};
        SemanticFrame insetFrame = frame;
        insetFrame.draws = std::span<const SemanticDraw>(&insetDraw, 1);
        SemanticFramePixels insetResult{};
        assert(pass.render_and_readback(insetFrame, insetResult, error) && error.empty());
        assert(hash(insetResult) != hash(first));
        require_color(pixel(insetResult, 5, 5), {});
        assert(pixel(insetResult, 7, 7).a > 0.9f);

        // Known-different control: revision and decoded bytes change together. A stale upload or
        // stale readback would incorrectly reproduce the baseline hash.
        rgba[0] = 0;
        rgba[2] = 255;
        image.revision = 1;
        draw.picture.material.textures[0].revision = 1;
        semanticDraw = draw;
        SemanticFramePixels changed{};
        assert(pass.render_and_readback(frame, changed, error) && error.empty());
        // Immutable revisions remain resident so recurring game assets do not allocate and upload
        // again every frame. The original and changed revisions are intentionally distinct keys.
        assert(pass.resident_image_count() == 4);
        assert(hash(changed) != hash(first));
        require_color(pixel(changed, 5, 5), {0, 0, 1, 1});

        // Mixed-order known-positive: solid red, opaque green picture, then solid blue. The last
        // operation must win at their shared overlap. Moving the green picture after blue must
        // produce the other answer, proving this is one ordered stream rather than family passes.
        const std::array<std::uint8_t, 4> greenPixel{0, 255, 0, 255};
        const DecodedImageView greenImage{
            .resource = 41, .width = 1, .height = 1, .rgba8 = greenPixel};
        PictureCommand greenPicture{};
        greenPicture.instance = 42;
        greenPicture.positions = {Vec2{4, 4}, Vec2{12, 4}, Vec2{4, 12}, Vec2{12, 12}};
        greenPicture.uv = {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}, Vec2{1, 1}};
        greenPicture.material.textureCount = 1;
        greenPicture.material.textures[0] =
            PictureTexture{.resource = 41, .width = 1, .height = 1, .hasAlpha = true};
        const GlyphCommand greenGlyph{.instance = 42,
                                      .code = 'A',
                                      .positions = greenPicture.positions,
                                      .uv = greenPicture.uv,
                                      .corner = greenPicture.corner,
                                      .atlas = greenPicture.material.textures[0]};
        const GlyphDraw greenDraw{draw.canvas, greenGlyph};
        const SolidRectangleDraw redFill = solid(40, 0, 0, 16, 16, {1, 0, 0, 1});
        const SolidRectangleDraw blueFill = solid(43, 8, 8, 16, 16, {0, 0, 1, 1});
        const std::array<SemanticDraw, 3> mixedDraws{redFill, greenDraw, blueFill};
        const SemanticFrame mixedFrame{.targetWidth = 16,
                                       .targetHeight = 16,
                                       .draws = mixedDraws,
                                       .images = std::span<const DecodedImageView>(&greenImage, 1)};
        SemanticFramePixels mixed{};
        assert(pass.render_and_readback(mixedFrame, mixed, error) && error.empty());
        require_color(pixel(mixed, 6, 6), {0, 1, 0, 1});
        require_color(pixel(mixed, 10, 10), {0, 0, 1, 1});

        const std::array<SemanticDraw, 3> reorderedDraws{redFill, blueFill, greenDraw};
        SemanticFrame reorderedFrame = mixedFrame;
        reorderedFrame.draws = reorderedDraws;
        SemanticFramePixels reordered{};
        assert(pass.render_and_readback(reorderedFrame, reordered, error) && error.empty());
        assert(hash(reordered) != hash(mixed));
        require_color(pixel(reordered, 10, 10), {0, 1, 0, 1});

        // Solid-family controls: an out-of-clip fill is a no-op and half alpha blends with the
        // existing red clear rather than replacing it opaquely.
        SolidRectangleDraw clippedSolid = blueFill;
        clippedSolid.rectangle.clip = {.enabled = true, .x = 20, .y = 20, .width = 2, .height = 2};
        const std::array<SemanticDraw, 2> noOpDraws{redFill, clippedSolid};
        SemanticFrame noOpFrame{.targetWidth = 16, .targetHeight = 16, .draws = noOpDraws};
        SemanticFramePixels noOp{};
        assert(pass.render_and_readback(noOpFrame, noOp, error) && error.empty());
        require_color(pixel(noOp, 10, 10), {1, 0, 0, 1});
        const std::array<SemanticDraw, 2> alphaDraws{redFill,
                                                     solid(44, 0, 0, 16, 16, {0, 0, 1, 0.5f})};
        SemanticFrame alphaFrame{.targetWidth = 16, .targetHeight = 16, .draws = alphaDraws};
        SemanticFramePixels alpha{};
        assert(pass.render_and_readback(alphaFrame, alpha, error) && error.empty());
        const Color alphaBlend = pixel(alpha, 10, 10);
        assert(alphaBlend.r > 0.70f && alphaBlend.b > 0.70f && alphaBlend.g < 0.01f);
    }

    target.shutdown();
    assert(platform.shutdown(platformError));

    // Exercise the production live-client path, including the exact bridge lease, offscreen
    // device-only platform, fenced submission, readback, and duplicate-consume refusal. The empty
    // frame is the required negative control; the known picture must produce the other answer.
    auto& sharedPlatform = sb::native_render::sdl_gpu_platform();
    auto& bridge = sb::native_render::semantic_frame_bridge();
    auto& client = sb::native_render::sdl_semantic_frame_client();
    assert(sb::native_render::parse_semantic_frame_mode(nullptr) ==
           sb::native_render::SemanticFrameMode::Disabled);
    assert(sb::native_render::parse_semantic_frame_mode("off") ==
           sb::native_render::SemanticFrameMode::Disabled);
    assert(sb::native_render::parse_semantic_frame_mode("audit") ==
           sb::native_render::SemanticFrameMode::Audit);
    assert(sb::native_render::parse_semantic_frame_mode("preview") ==
           sb::native_render::SemanticFrameMode::Preview);
    assert(sb::native_render::parse_semantic_frame_mode("") ==
           sb::native_render::SemanticFrameMode::Invalid);
    assert(sharedPlatform.initialize_device({}, platformError));
    assert(!sharedPlatform.presenter_ready());
    assert(client.initialize(sharedPlatform, bridge,
                             {.width = 16,
                              .height = 16,
                              .readback = sb::native_render::SemanticReadbackMode::EveryFrame},
                             platformError));
    assert(bridge.begin());
    assert(bridge.seal());
    assert(client.encode_last_sealed(platformError));
    const std::uint64_t clearHash = client.stats().lastSampleHash;
    assert(client.stats().sampledFrames == 1);
    assert(client.stats().lastSampleNonClearPixels == 0);
    assert(!client.validate_output(platformError));
    assert(platformError.find("never observed pixels") != std::string::npos);

    // Known-positive 3D control through the production collector/client: a red clip-space triangle
    // must produce pixels before any 2D draw exists. Moving it fully outside clip space is the
    // corresponding no-signal geometry control covered by the pure transform test.
    const std::array<MeshVertex, 3> modelVertices{MeshVertex{{-0.75F, -0.75F, 0.5F}, {0, 0}},
                                                  MeshVertex{{0.75F, -0.75F, 0.5F}, {1, 0}},
                                                  MeshVertex{{0.0F, 0.75F, 0.5F}, {0.5F, 1}}};
    const MeshResourceView modelMesh{71, 1, modelVertices};
    const ModelDraw modelDraw{
        .instance = 72,
        .mesh = {.resource = 71, .revision = 1, .vertexCount = 3},
        .pose = {.modelViews = {sb::native_render::Matrix3x4{
                     .value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}},
                 .count = 1},
        .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
        .material = sb::native_render::UnlitColorMaterial{.baseColor = {1, 0, 0, 1}},
    };
    assert(bridge.begin());
    assert(sb::native_render::submit_model(modelDraw, modelMesh));
    assert(bridge.seal());
    assert(client.encode_last_sealed(platformError));
    assert(client.stats().submittedModels == 1 && client.stats().submittedMeshes == 1);
    assert(client.stats().submittedMeshVertices == 3);
    assert(client.stats().lastSampleNonClearPixels != 0);
    assert(client.stats().lastSampleHash != clearHash);
    const std::uint64_t redModelHash = client.stats().lastSampleHash;

    // The textured model path must show the decoded image rather than falling back to the color
    // shader. A one-pixel green image over the same white triangle has to produce a different
    // answer from both the black clear and the red untextured control.
    const std::array<std::uint8_t, 4> greenRgba{0, 255, 0, 255};
    const DecodedImageView greenImage{
        .resource = 73, .revision = 4, .width = 1, .height = 1, .rgba8 = greenRgba};
    ModelDraw texturedModel = modelDraw;
    texturedModel.instance = 74;
    texturedModel.material = sb::native_render::UnlitTexturedMaterial{
        .texture = {.resource = 73, .revision = 4, .width = 1, .height = 1},
        .usesVertexColor = false};
    assert(bridge.begin());
    assert(sb::native_render::submit_model(texturedModel, modelMesh,
                                           std::span<const DecodedImageView>(&greenImage, 1)));
    assert(bridge.seal());
    assert(client.encode_last_sealed(platformError));
    assert(client.stats().submittedModels == 2);
    assert(client.stats().lastSampleNonClearPixels != 0);
    assert(client.stats().lastSampleHash != clearHash);
    assert(client.stats().lastSampleHash != redModelHash);

    assert(bridge.begin());
    assert(sb::native_render::submit_picture(draw, std::span<const DecodedImageView>(&image, 1)));
    const GlyphCommand auditGlyph{.instance = 100,
                                  .code = 'A',
                                  .positions = draw.picture.positions,
                                  .uv = draw.picture.uv,
                                  .corner = draw.picture.corner,
                                  .atlas = draw.picture.material.textures[0]};
    assert(sb::native_render::submit_glyph(GlyphDraw{draw.canvas, auditGlyph},
                                           std::span<const DecodedImageView>(&image, 1)));
    SolidRectangleDraw auditFill = solid(99, 12, 12, 16, 16, {1, 1, 1, 1});
    auditFill.rectangle.source = sb::native_render::SolidRectangleSource::J2dGrafContextFillBox;
    assert(sb::native_render::submit_solid_rectangle(auditFill));
    assert(bridge.seal());
    assert(client.encode_last_sealed(platformError));
    assert(client.stats().submittedFrames == 4 && client.stats().completedFrames == 4);
    assert(client.stats().nonEmptyFrames == 3 && client.stats().mixedOperationFrames == 1);
    assert(client.stats().submittedOperations == 5);
    assert(client.stats().submittedPictures == 1 && client.stats().submittedGlyphs == 1 &&
           client.stats().submittedSolidRectangles == 1);
    assert(client.stats().submittedJ2dFillBoxes == 1);
    assert(client.stats().lastSampleNonClearPixels != 0);
    assert(client.stats().lastSampleHash != clearHash);
    assert(client.validate_output(platformError));
    assert(!client.encode_last_sealed(platformError));
    assert(platformError.find("already consumed") != std::string::npos);

    assert(client.shutdown(platformError));
    assert(!bridge.active() && !client.ready());
    assert(sharedPlatform.shutdown(platformError));

    // Preview startup refuses a hidden/headless window instead of running a plausible-looking
    // present loop that can never display anything.
    assert(sharedPlatform.initialize_device({}, platformError));
    assert(!client.initialize(sharedPlatform, bridge,
                              {.width = 16,
                               .height = 16,
                               .readback = sb::native_render::SemanticReadbackMode::None,
                               .presentationWindow = window},
                              platformError));
    assert(platformError.find("requires a visible") != std::string::npos);
    assert(!client.ready() && !sharedPlatform.presenter_ready());

    // The preview arm must visibly submit the semantic target through the production presenter,
    // while a window that becomes hidden is reported as unavailable without dropping the frame.
    assert(SDL_ShowWindow(window));
    assert(client.initialize(sharedPlatform, bridge,
                             {.width = 16,
                              .height = 16,
                              .readback = sb::native_render::SemanticReadbackMode::None,
                              .presentationWindow = window},
                             platformError));
    assert(sharedPlatform.presenter_ready());
    assert(bridge.begin());
    assert(sb::native_render::submit_picture(draw, std::span<const DecodedImageView>(&image, 1)));
    assert(bridge.seal());
    assert(client.encode_last_sealed(platformError));
    assert(client.stats().presentedFrames == 1);
    assert(client.stats().windowUnavailableFrames == 0);

    assert(SDL_HideWindow(window));
    assert(bridge.begin());
    assert(sb::native_render::submit_picture(draw, std::span<const DecodedImageView>(&image, 1)));
    assert(bridge.seal());
    assert(client.encode_last_sealed(platformError));
    assert(client.stats().presentedFrames == 1);
    assert(client.stats().windowUnavailableFrames == 1);
    assert(client.shutdown(platformError));
    assert(sharedPlatform.shutdown(platformError));
    SDL_DestroyWindow(window);
    SDL_Quit();
}
