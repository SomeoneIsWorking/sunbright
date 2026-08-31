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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using sb::native_render::Color;
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

Color pixel(const SemanticFramePixels& frame, std::uint32_t x, std::uint32_t y) {
    const std::size_t offset = (static_cast<std::size_t>(y) * frame.width + x) * 4;
    constexpr float scale = 1.0f / 255.0f;
    return {frame.rgba8[offset] * scale, frame.rgba8[offset + 1] * scale,
            frame.rgba8[offset + 2] * scale, frame.rgba8[offset + 3] * scale};
}

bool near(float actual, float expected, float tolerance = 2.0f / 255.0f) {
    return actual >= expected - tolerance && actual <= expected + tolerance;
}

float srgb_to_linear(float value) {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

float linear_to_srgb(float value) {
    return value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

void require_color(Color actual, Color expected) {
    assert(near(actual.r, expected.r));
    assert(near(actual.g, expected.g));
    assert(near(actual.b, expected.b));
    assert(near(actual.a, expected.a));
}

std::uint64_t hash(const SemanticFramePixels& frame) {
    std::uint64_t value = 1469598103934665603ULL;
    for (std::uint8_t byte : frame.rgba8) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

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

bool encode_and_readback(Semantic2dPass& pass, const SemanticFrame& frame,
                         const SdlGpuFrameTarget& target, SemanticFramePixels& output,
                         std::string& error) {
    SDL_GPUDevice* device = target.device();
    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    const std::size_t byteCount =
        static_cast<std::size_t>(frame.targetWidth) * frame.targetHeight * 4;
    const SDL_GPUTransferBufferCreateInfo downloadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                       static_cast<Uint32>(byteCount), 0};
    SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(device, &downloadInfo);
    if (commandBuffer == nullptr || download == nullptr) {
        if (commandBuffer != nullptr)
            SDL_CancelGPUCommandBuffer(commandBuffer);
        if (download != nullptr)
            SDL_ReleaseGPUTransferBuffer(device, download);
        error = std::string("GPU control resource creation failed: ") + SDL_GetError();
        return false;
    }
    const sb::native_render::Semantic2dPassTarget passTarget{
        commandBuffer, target.color(), target.desc().colorFormat, SDL_GPU_LOADOP_CLEAR,
        SDL_GPU_STOREOP_STORE};
    if (!pass.encode(frame, passTarget, error)) {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        std::string completionError;
        (void)pass.complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commandBuffer);
    if (copy == nullptr) {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        std::string completionError;
        (void)pass.complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(device, download);
        error = std::string("GPU control copy pass failed: ") + SDL_GetError();
        return false;
    }
    const SDL_GPUTextureRegion source{target.color(),     0, 0, 0, 0, 0, frame.targetWidth,
                                      frame.targetHeight, 1};
    const SDL_GPUTextureTransferInfo destination{download, 0, frame.targetWidth,
                                                 frame.targetHeight};
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    if (fence == nullptr) {
        std::string completionError;
        (void)pass.complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(device, download);
        error = std::string("GPU control submit failed: ") + SDL_GetError();
        return false;
    }
    if (!pass.complete_encode(true, error)) {
        SDL_ReleaseGPUFence(device, fence);
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUFence* fences[] = {fence};
    const bool waited = SDL_WaitForGPUFences(device, true, fences, 1);
    SDL_ReleaseGPUFence(device, fence);
    if (!waited) {
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device, download, false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    output = {frame.targetWidth, frame.targetHeight, std::vector<std::uint8_t>(byteCount)};
    std::memcpy(output.rgba8.data(), mapped, byteCount);
    SDL_UnmapGPUTransferBuffer(device, download);
    SDL_ReleaseGPUTransferBuffer(device, download);
    return true;
}

bool encode_3d_and_readback(Semantic3dPass& pass, const SemanticFrame& frame,
                            const SdlGpuFrameTarget& target, SemanticFramePixels& output,
                            std::string& error) {
    SDL_GPUDevice* device = target.device();
    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    const std::size_t byteCount =
        static_cast<std::size_t>(frame.targetWidth) * frame.targetHeight * 4;
    const SDL_GPUTransferBufferCreateInfo downloadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                       static_cast<Uint32>(byteCount), 0};
    SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(device, &downloadInfo);
    if (commandBuffer == nullptr || download == nullptr) {
        if (commandBuffer != nullptr)
            SDL_CancelGPUCommandBuffer(commandBuffer);
        if (download != nullptr)
            SDL_ReleaseGPUTransferBuffer(device, download);
        error = SDL_GetError();
        return false;
    }
    const sb::native_render::Semantic3dPassTarget passTarget{
        commandBuffer, target.color(), target.desc().colorFormat, target.depth(),
        target.desc().depthFormat};
    if (!pass.encode(frame, passTarget, error)) {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        std::string completionError;
        (void)pass.complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commandBuffer);
    if (copy == nullptr) {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        std::string completionError;
        (void)pass.complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(device, download);
        error = SDL_GetError();
        return false;
    }
    const SDL_GPUTextureRegion source{target.color(),     0, 0, 0, 0, 0, frame.targetWidth,
                                      frame.targetHeight, 1};
    const SDL_GPUTextureTransferInfo destination{download, 0, frame.targetWidth,
                                                 frame.targetHeight};
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    if (fence == nullptr) {
        std::string completionError;
        (void)pass.complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(device, download);
        error = SDL_GetError();
        return false;
    }
    if (!pass.complete_encode(true, error)) {
        SDL_ReleaseGPUFence(device, fence);
        SDL_ReleaseGPUTransferBuffer(device, download);
        return false;
    }
    SDL_GPUFence* fences[] = {fence};
    const bool waited = SDL_WaitForGPUFences(device, true, fences, 1);
    SDL_ReleaseGPUFence(device, fence);
    if (!waited) {
        SDL_ReleaseGPUTransferBuffer(device, download);
        error = SDL_GetError();
        return false;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device, download, false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, download);
        error = SDL_GetError();
        return false;
    }
    output = {frame.targetWidth, frame.targetHeight, std::vector<std::uint8_t>(byteCount)};
    std::memcpy(output.rgba8.data(), mapped, byteCount);
    SDL_UnmapGPUTransferBuffer(device, download);
    SDL_ReleaseGPUTransferBuffer(device, download);
    return true;
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
        assert(pass.resident_image_count() == 2);
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
        // image at 8-bit precision. Changing only that alpha from 255 to 128 must move the
        // shipping four-image shader from red to blue while its black ramp and zero highlights
        // stay inert.
        const std::array<std::uint8_t, 4> maskedPrimaryTexel{255, 0, 0, 255};
        std::array<std::uint8_t, 4> maskedMaskTexel{0, 0, 0, 255};
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
            .raster = {.cull = sb::native_render::ModelCullMode::None},
        };
        const SemanticFramePixels maskedPrimary =
            render(std::span<const ModelDraw>(&maskedModel, 1), maskedImages);
        const Color maskedPrimaryPixel = pixel(maskedPrimary, 8, 8);
        assert(maskedPrimaryPixel.r > 0.9F && maskedPrimaryPixel.b < 0.01F);
        maskedMaskTexel[3] = 128;
        maskedImages[1].revision = 2;
        std::get<sb::native_render::LitMaskedToonMaterial>(maskedModel.material)
            .maskTexture.revision = 2;
        const SemanticFramePixels maskedAlternate =
            render(std::span<const ModelDraw>(&maskedModel, 1), maskedImages);
        const Color maskedAlternatePixel = pixel(maskedAlternate, 8, 8);
        assert(maskedAlternatePixel.r < 0.01F && maskedAlternatePixel.b > 0.9F);
        assert(hash(maskedPrimary) != hash(maskedAlternate));

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
