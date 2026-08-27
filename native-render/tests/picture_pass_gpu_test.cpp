#include <sunbright/native_render/picture_pass.h>

#include <SDL3/SDL.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

using sb::native_render::Color;
using sb::native_render::PictureCommand;
using sb::native_render::PictureFrame;
using sb::native_render::PictureFramePixels;
using sb::native_render::PictureImage;
using sb::native_render::PicturePass;
using sb::native_render::PictureTexture;
using sb::native_render::Vec2;

Color pixel(const PictureFramePixels& frame, std::uint32_t x, std::uint32_t y) {
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

std::uint64_t hash(const PictureFramePixels& frame) {
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

} // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SKIP: SDL video unavailable: " << SDL_GetError() << '\n';
        return 77;
    }
    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
    if (device == nullptr) {
        std::cerr << "SKIP: SDL GPU device unavailable: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 77;
    }

    // 2x2 RGBA: red, green / blue, half-alpha white. Nearest sampling makes each quadrant an
    // unmistakable known-positive; the clip rectangle proves the production scissor conversion.
    std::array<std::uint8_t, 16> rgba{255, 0, 0,   255, 0,   255, 0,   255,
                                      0,   0, 255, 255, 255, 255, 255, 128};
    PictureImage image{.resource = 9, .width = 2, .height = 2, .rgba8 = rgba};
    PictureCommand picture = command();
    const PictureFrame frame{
        .canvas = {.origin = {0, 0}, .extent = {16, 16}, .targetWidth = 16, .targetHeight = 16},
        .commands = std::span<const PictureCommand>(&picture, 1),
        .images = std::span<const PictureImage>(&image, 1)};

    {
        // The renderer client must release its pipeline/shaders before the host destroys the one
        // shared device. Vulkan validation is enabled specifically to enforce that ownership.
        PicturePass pass(device);
        std::string error;
        PictureFramePixels first{};
        assert(pass.render_and_readback(frame, first, error) && error.empty());
        require_color(pixel(first, 1, 1), {});
        require_color(pixel(first, 5, 5), {1, 0, 0, 1});
        require_color(pixel(first, 10, 5), {0, 1, 0, 1});
        require_color(pixel(first, 5, 10), {0, 0, 1, 1});
        const Color alphaPixel = pixel(first, 10, 10);
        assert(alphaPixel.r > 0.70f && alphaPixel.g > 0.70f && alphaPixel.b > 0.70f);
        assert(near(alphaPixel.a, 128.0f / 255.0f));

        PictureFramePixels repeated{};
        assert(pass.render_and_readback(frame, repeated, error) && error.empty());
        assert(hash(repeated) == hash(first));

        // No-op control: a valid draw wholly outside its semantic clip must not perturb the target
        // or be misclassified as a malformed command.
        PictureCommand clipped = picture;
        clipped.instance = 2;
        clipped.clip = {.enabled = true, .x = 32, .y = 32, .width = 4, .height = 4};
        const std::array clippedCommands{picture, clipped};
        PictureFrame clippedFrame = frame;
        clippedFrame.commands = clippedCommands;
        PictureFramePixels clippedResult{};
        assert(pass.render_and_readback(clippedFrame, clippedResult, error) && error.empty());
        assert(hash(clippedResult) == hash(first));

        // Known-different control: revision and decoded bytes change together. A stale upload or
        // stale readback would incorrectly reproduce the baseline hash.
        rgba[0] = 0;
        rgba[2] = 255;
        image.revision = 1;
        picture.material.textures[0].revision = 1;
        PictureFramePixels changed{};
        assert(pass.render_and_readback(frame, changed, error) && error.empty());
        assert(hash(changed) != hash(first));
        require_color(pixel(changed, 5, 5), {0, 0, 1, 1});
    }

    SDL_DestroyGPUDevice(device);
    SDL_Quit();
}
