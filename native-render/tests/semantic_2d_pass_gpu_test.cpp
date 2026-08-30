#include <sunbright/native_render/sdl_gpu_frame_target.h>
#include <sunbright/native_render/sdl_semantic_frame_client.h>
#include <sunbright/native_render/semantic_2d_pass.h>
#include <sunbright/native_render/semantic_sink.h>

#include <SDL3/SDL.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using sb::native_render::Color;
using sb::native_render::DecodedImageView;
using sb::native_render::GlyphCommand;
using sb::native_render::GlyphDraw;
using sb::native_render::PictureCommand;
using sb::native_render::PictureDraw;
using sb::native_render::PictureTexture;
using sb::native_render::SdlGpuFrameTarget;
using sb::native_render::SdlGpuPlatform;
using sb::native_render::Semantic2dPass;
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
        assert(pass.resident_image_count() == 1);
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
        const SemanticFrame mixedFrame{
            16, 16, mixedDraws, std::span<const DecodedImageView>(&greenImage, 1), {}};
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
        SemanticFrame noOpFrame{16, 16, noOpDraws, {}, {}};
        SemanticFramePixels noOp{};
        assert(pass.render_and_readback(noOpFrame, noOp, error) && error.empty());
        require_color(pixel(noOp, 10, 10), {1, 0, 0, 1});
        const std::array<SemanticDraw, 2> alphaDraws{redFill,
                                                     solid(44, 0, 0, 16, 16, {0, 0, 1, 0.5f})};
        SemanticFrame alphaFrame{16, 16, alphaDraws, {}, {}};
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
    assert(sb::native_render::parse_semantic_frame_audit(nullptr) ==
           sb::native_render::SemanticFrameAuditSetting::Disabled);
    assert(sb::native_render::parse_semantic_frame_audit("0") ==
           sb::native_render::SemanticFrameAuditSetting::Disabled);
    assert(sb::native_render::parse_semantic_frame_audit("1") ==
           sb::native_render::SemanticFrameAuditSetting::Enabled);
    assert(sb::native_render::parse_semantic_frame_audit("") ==
           sb::native_render::SemanticFrameAuditSetting::Invalid);
    assert(sb::native_render::parse_semantic_frame_audit("yes") ==
           sb::native_render::SemanticFrameAuditSetting::Invalid);
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
    assert(!client.validate_audit(platformError));
    assert(platformError.find("never observed output") != std::string::npos);

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
    assert(client.stats().submittedFrames == 2 && client.stats().completedFrames == 2);
    assert(client.stats().nonEmptyFrames == 1 && client.stats().mixedOperationFrames == 1);
    assert(client.stats().submittedOperations == 3);
    assert(client.stats().submittedPictures == 1 && client.stats().submittedGlyphs == 1 &&
           client.stats().submittedSolidRectangles == 1);
    assert(client.stats().submittedJ2dFillBoxes == 1);
    assert(client.stats().lastSampleNonClearPixels != 0);
    assert(client.stats().lastSampleHash != clearHash);
    assert(client.validate_audit(platformError));
    assert(!client.encode_last_sealed(platformError));
    assert(platformError.find("already consumed") != std::string::npos);

    assert(client.shutdown(platformError));
    assert(!bridge.active() && !client.ready());
    assert(sharedPlatform.shutdown(platformError));
    SDL_DestroyWindow(window);
    SDL_Quit();
}
