#include "semantic_particle_adapter.h"

#include "guest_byte_reader.h"
#include "semantic_j3d_scene.h"

#include <sunbright/native_render/model_context.h>
#include <sunbright/native_render/particle_billboard.h>
#include <sunbright/native_render/res_timg_decode.h>
#include <sunbright/native_render/semantic_sink.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include <lucent/log.h>

namespace sb::recomp {
namespace {

constexpr std::uint32_t kDrawContextPcbSdaOffset = 0x5AD8;
constexpr std::uint32_t kPcbHalfExtent = 0x04;
constexpr std::uint32_t kPcbPivot = 0x0C;
constexpr std::uint32_t kPcbTexCoords = 0x14;
constexpr std::uint32_t kPcbViewMatrix = 0x34;
constexpr std::uint32_t kContextBaseShape = 0x04;
constexpr std::uint32_t kContextTextureResource = 0x1C;
constexpr std::uint32_t kParticleStatus = 0x10;
constexpr std::uint32_t kParticlePosition = 0x2C;
constexpr std::uint32_t kParticleDrawParams = 0xA0;
constexpr std::uint32_t kDrawScaleX = 0x10;
constexpr std::uint32_t kDrawScaleY = 0x14;
constexpr std::uint32_t kDrawTextureIndex = 0x3A;
constexpr std::uint32_t kShapeTev = 0x48;
constexpr std::uint32_t kShapeType = 0x69;
constexpr std::uint32_t kShapeBlendMode = 0x6D;
constexpr std::uint32_t kShapeBlendSource = 0x6E;
constexpr std::uint32_t kShapeBlendDestination = 0x6F;
constexpr std::uint32_t kShapeAlphaCompare0 = 0x71;
constexpr std::uint32_t kShapeAlphaReference0 = 0x72;
constexpr std::uint32_t kShapeAlphaOperation = 0x73;
constexpr std::uint32_t kShapeAlphaCompare1 = 0x74;
constexpr std::uint32_t kShapeAlphaReference1 = 0x75;
constexpr std::uint32_t kShapeDepthEnabled = 0x77;
constexpr std::uint32_t kShapeDepthCompare = 0x78;
constexpr std::uint32_t kShapeDepthWrite = 0x79;
constexpr std::uint32_t kTextureCount = 0x24;
constexpr std::uint32_t kTextureArray = 0x2C;
constexpr std::uint32_t kTextureRawData = 0x04;
constexpr std::uint32_t kTextureResTimg = 0x20;

struct ReadContext {
    BigEndianGuestReader reader;
};

struct Stats {
    std::uint64_t submitted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t invisible = 0;
    std::uint64_t unsupportedShape = 0;
    std::uint64_t unsupportedType = 0;
    std::uint64_t unsupportedProgram = 0;
    std::uint64_t invalidTexture = 0;
    std::uint64_t decodeFailures = 0;
    std::uint64_t noScene = 0;
    std::uint64_t viewFailures = 0;
};

Stats g_stats{};

bool add_address(std::uint32_t base, std::uint32_t offset, std::uint32_t& result) noexcept {
    if (offset > std::numeric_limits<std::uint32_t>::max() - base)
        return false;
    result = base + offset;
    return true;
}

bool read_asset(native_render::ByteAddress address, std::span<std::uint8_t> output,
                void* context) noexcept {
    std::uint64_t numeric = 0;
    if (!address.guest_value(numeric) || numeric > std::numeric_limits<std::uint32_t>::max())
        return false;
    return static_cast<ReadContext*>(context)->reader.bytes(static_cast<std::uint32_t>(numeric),
                                                            output.data(), output.size());
}

bool read_f32(const BigEndianGuestReader& reader, std::uint32_t base, std::uint32_t offset,
              float& value) noexcept {
    std::uint32_t address = 0;
    return add_address(base, offset, address) && reader.f32(address, value);
}

bool read_u8(const BigEndianGuestReader& reader, std::uint32_t base, std::uint32_t offset,
             std::uint8_t& value) noexcept {
    std::uint32_t address = 0;
    return add_address(base, offset, address) && reader.u8(address, value);
}

bool read_u32(const BigEndianGuestReader& reader, std::uint32_t base, std::uint32_t offset,
              std::uint32_t& value) noexcept {
    std::uint32_t address = 0;
    return add_address(base, offset, address) && reader.u32(address, value);
}

bool read_texture_program(const BigEndianGuestReader& reader, std::uint32_t shape,
                          std::array<std::uint32_t, 4>& args) noexcept {
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (!read_u32(reader, shape, kShapeTev + static_cast<std::uint32_t>(index * 4U),
                      args[index]))
            return false;
    }
    return true;
}

enum class TextureProgram : std::uint8_t { Unsupported, Direct, Modulated };

float jpa_u8_thre(std::uint8_t left, std::uint8_t right) noexcept {
    const std::uint32_t product =
        static_cast<std::uint32_t>(left) * (static_cast<std::uint32_t>(right) + 1U);
    return static_cast<float>((product * 0x10000U) >> 24U) / 255.0F;
}

TextureProgram classify_texture_program(const std::array<std::uint32_t, 4>& args) noexcept {
    if (args == std::array<std::uint32_t, 4>{15U, 8U, 12U, 15U})
        return TextureProgram::Direct;
    if (args == std::array<std::uint32_t, 4>{15U, 2U, 8U, 15U})
        return TextureProgram::Modulated;
    return TextureProgram::Unsupported;
}

bool build_raster_policy(const BigEndianGuestReader& reader, std::uint32_t shape,
                         native_render::ModelRasterPolicy& policy) noexcept {
    std::uint8_t depthEnabled = 0;
    std::uint8_t depthCompare = 0;
    std::uint8_t depthWrite = 0;
    std::uint8_t alphaCompare0 = 0;
    std::uint8_t alphaReference0 = 0;
    std::uint8_t alphaOperation = 0;
    std::uint8_t alphaCompare1 = 0;
    std::uint8_t alphaReference1 = 0;
    std::uint8_t blendMode = 0;
    std::uint8_t blendSource = 0;
    std::uint8_t blendDestination = 0;
    if (!read_u8(reader, shape, kShapeDepthEnabled, depthEnabled) ||
        !read_u8(reader, shape, kShapeDepthCompare, depthCompare) ||
        !read_u8(reader, shape, kShapeDepthWrite, depthWrite) ||
        !read_u8(reader, shape, kShapeAlphaCompare0, alphaCompare0) ||
        !read_u8(reader, shape, kShapeAlphaReference0, alphaReference0) ||
        !read_u8(reader, shape, kShapeAlphaOperation, alphaOperation) ||
        !read_u8(reader, shape, kShapeAlphaCompare1, alphaCompare1) ||
        !read_u8(reader, shape, kShapeAlphaReference1, alphaReference1) ||
        !read_u8(reader, shape, kShapeBlendMode, blendMode) ||
        !read_u8(reader, shape, kShapeBlendSource, blendSource) ||
        !read_u8(reader, shape, kShapeBlendDestination, blendDestination))
        return false;

    native_render::ModelRasterPolicy result{};
    if (depthEnabled != 0 && depthCompare != 2U)
        return false;
    result.depthTest = depthEnabled != 0;
    result.depthCompare = result.depthTest ? native_render::ModelDepthCompare::LessOrEqual
                                           : native_render::ModelDepthCompare::Always;
    result.depthWrite = depthWrite != 0;
    if (alphaCompare0 == 7U && alphaCompare1 == 7U &&
        (alphaOperation == 0U || alphaOperation == 1U || alphaOperation == 3U)) {
        result.alphaTest = native_render::ModelAlphaTest::PassAll;
    } else if (alphaCompare0 == 5U && alphaReference0 == 0x80U && alphaCompare1 == 2U &&
               alphaReference1 == 0xFFU && alphaOperation == 0U) {
        result.alphaTest = native_render::ModelAlphaTest::GreaterOrEqualHalf;
    } else {
        return false;
    }
    if (blendMode == 0U) {
        if (blendSource != 1U || blendDestination != 0U)
            return false;
        result.blend = native_render::ModelBlendMode::Replace;
    } else if (blendMode == 1U && blendSource == 4U && blendDestination == 5U) {
        result.blend = native_render::ModelBlendMode::SourceAlpha;
    } else if (blendMode == 1U && blendSource == 4U && blendDestination == 1U) {
        result.depthTest = false;
        result.depthWrite = false;
        result.depthCompare = native_render::ModelDepthCompare::Always;
        result.blend = native_render::ModelBlendMode::Additive;
    } else if (blendMode == 1U && blendSource == 6U && blendDestination == 7U) {
        result.blend = native_render::ModelBlendMode::DestinationAlpha;
    } else {
        return false;
    }
    policy = result;
    return true;
}

bool capture_texture(const BigEndianGuestReader& reader, std::uint32_t resource,
                     std::uint16_t textureIndex, native_render::DecodedTexture& decoded) noexcept {
    std::uint32_t count = 0;
    std::uint32_t array = 0;
    if (!read_u32(reader, resource, kTextureCount, count) ||
        !read_u32(reader, resource, kTextureArray, array) || array == 0 || textureIndex >= count ||
        textureIndex > (std::numeric_limits<std::uint32_t>::max() - array) / 4U)
        return false;
    std::uint32_t texture = 0;
    if (!reader.u32(array + static_cast<std::uint32_t>(textureIndex) * 4U, texture) || texture == 0)
        return false;
    std::uint32_t rawData = 0;
    std::uint32_t header = 0;
    if (!read_u32(reader, texture, kTextureRawData, rawData) || rawData == 0 ||
        !add_address(rawData, kTextureResTimg, header))
        return false;
    ReadContext context{reader};
    const auto error = native_render::decode_res_timg(
        {read_asset, &context}, native_render::ByteAddress::guest(header),
        (static_cast<std::uint64_t>(resource) << 32U) | header, decoded);
    return error == native_render::ResTimgDecodeError::None;
}

} // namespace

bool submit_guest_particle_billboard(std::uint32_t drawContext, std::uint32_t particle,
                                     std::uint32_t smallDataBase) noexcept {
    if (!native_render::has_semantic_sink() || drawContext == 0 || particle == 0)
        return false;
    const auto* scene = current_semantic_j3d_scene();
    if (scene == nullptr || scene->projectionKind != native_render::ProjectionKind::Perspective) {
        ++g_stats.noScene;
        return false;
    }
    const BigEndianGuestReader reader(live_guest_byte_reader());
    std::uint32_t shape = 0;
    std::uint32_t resource = 0;
    std::uint8_t type = 0;
    std::uint32_t status = 0;
    if (!read_u32(reader, drawContext, kContextBaseShape, shape) || shape == 0 ||
        !read_u32(reader, drawContext, kContextTextureResource, resource) || resource == 0 ||
        !read_u8(reader, shape, kShapeType, type) ||
        !read_u32(reader, particle, kParticleStatus, status)) {
        ++g_stats.rejected;
        return false;
    }
    if ((status & 0x8U) != 0) {
        ++g_stats.invisible;
        return false;
    }
    if (type != 2U) {
        ++g_stats.unsupportedShape;
        ++g_stats.unsupportedType;
        return false;
    }
    std::array<std::uint32_t, 4> program{};
    if (!read_texture_program(reader, shape, program) ||
        classify_texture_program(program) == TextureProgram::Unsupported) {
        ++g_stats.unsupportedShape;
        ++g_stats.unsupportedProgram;
        return false;
    }

    std::uint32_t pcb = 0;
    std::uint32_t view = 0;
    if (smallDataBase < kDrawContextPcbSdaOffset) {
        ++g_stats.viewFailures;
        ++g_stats.rejected;
        return false;
    }
    const std::uint32_t drawContextPcb = smallDataBase - kDrawContextPcbSdaOffset;
    const bool pcbReadable = reader.u32(drawContextPcb, pcb);
    const bool viewReadable =
        pcbReadable && pcb != 0 && read_u32(reader, pcb, kPcbViewMatrix, view);
    if (!pcbReadable || pcb == 0 || !viewReadable || view == 0) {
        ++g_stats.viewFailures;
        ++g_stats.rejected;
        return false;
    }
    std::array<float, 12> viewMatrix{};
    for (std::size_t index = 0; index < viewMatrix.size(); ++index) {
        if (!reader.f32(view + static_cast<std::uint32_t>(index * 4U), viewMatrix[index])) {
            ++g_stats.viewFailures;
            ++g_stats.rejected;
            return false;
        }
    }

    float worldX = 0.0F;
    float worldY = 0.0F;
    float worldZ = 0.0F;
    float scaleX = 0.0F;
    float scaleY = 0.0F;
    float halfX = 0.0F;
    float halfY = 0.0F;
    float pivotX = 0.0F;
    float pivotY = 0.0F;
    if (!read_f32(reader, particle, kParticlePosition, worldX) ||
        !read_f32(reader, particle, kParticlePosition + 4U, worldY) ||
        !read_f32(reader, particle, kParticlePosition + 8U, worldZ) ||
        !read_f32(reader, particle, kParticleDrawParams + kDrawScaleX, scaleX) ||
        !read_f32(reader, particle, kParticleDrawParams + kDrawScaleY, scaleY) ||
        !read_f32(reader, pcb, kPcbHalfExtent, halfX) ||
        !read_f32(reader, pcb, kPcbHalfExtent + 4U, halfY) ||
        !read_f32(reader, pcb, kPcbPivot, pivotX) ||
        !read_f32(reader, pcb, kPcbPivot + 4U, pivotY)) {
        ++g_stats.rejected;
        return false;
    }
    if (halfX != 0.0F)
        pivotX /= halfX;
    else
        pivotX = 0.0F;
    if (halfY != 0.0F)
        pivotY /= halfY;
    else
        pivotY = 0.0F;

    native_render::ParticleBillboardInput input{};
    input.eyeCenter = {
        viewMatrix[0] * worldX + viewMatrix[1] * worldY + viewMatrix[2] * worldZ + viewMatrix[3],
        viewMatrix[4] * worldX + viewMatrix[5] * worldY + viewMatrix[6] * worldZ + viewMatrix[7],
        viewMatrix[8] * worldX + viewMatrix[9] * worldY + viewMatrix[10] * worldZ + viewMatrix[11]};
    input.halfExtent = {scaleX * halfX, scaleY * halfY};
    input.pivot = {pivotX, pivotY};
    for (std::size_t index = 0; index < input.uv.size(); ++index) {
        if (!reader.f32(pcb + kPcbTexCoords + static_cast<std::uint32_t>(index * 8U),
                        input.uv[index].x) ||
            !reader.f32(pcb + kPcbTexCoords + static_cast<std::uint32_t>(index * 8U + 4U),
                        input.uv[index].y)) {
            ++g_stats.rejected;
            return false;
        }
    }

    std::uint16_t textureIndex = 0;
    if (!reader.u16(particle + kParticleDrawParams + kDrawTextureIndex, textureIndex)) {
        ++g_stats.invalidTexture;
        return false;
    }
    native_render::DecodedTexture decoded{};
    if (!capture_texture(reader, resource, textureIndex, decoded)) {
        ++g_stats.decodeFailures;
        return false;
    }
    native_render::ModelRasterPolicy raster{};
    if (!build_raster_policy(reader, shape, raster)) {
        ++g_stats.rejected;
        return false;
    }

    const auto vertices = native_render::make_particle_billboard_mesh(input);
    const TextureProgram textureProgram = classify_texture_program(program);
    native_render::ModelMaterial material = native_render::UnlitTexturedMaterial{
        .texture = decoded.texture, .usesVertexColor = false, .raster = raster};
    if (textureProgram == TextureProgram::Modulated) {
        std::array<std::uint8_t, 4> particleColor{};
        std::array<std::uint8_t, 4> emitterColor{};
        for (std::size_t index = 0; index < particleColor.size(); ++index) {
            if (!reader.u8(particle + kParticleDrawParams + 0x2CU + index, particleColor[index]) ||
                !reader.u8(pcb + 0x98U + index, emitterColor[index])) {
                ++g_stats.rejected;
                return false;
            }
        }
        float alpha = 0.0F;
        if (!read_f32(reader, particle, kParticleDrawParams + 0x20U, alpha)) {
            ++g_stats.rejected;
            return false;
        }
        material = native_render::TexturedEffectMaterial{
            .texture = decoded.texture,
            .modulation = {jpa_u8_thre(particleColor[0], emitterColor[0]),
                           jpa_u8_thre(particleColor[1], emitterColor[1]),
                           jpa_u8_thre(particleColor[2], emitterColor[2]),
                           alpha * jpa_u8_thre(particleColor[3], emitterColor[3])},
            .raster = raster};
    }
    native_render::ModelDraw draw{};
    draw.instance = particle;
    draw.mesh = {particle, native_render::mesh_revision(vertices),
                 static_cast<std::uint32_t>(vertices.size())};
    draw.pose.count = 1;
    draw.pose.modelViews[0].value = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                     0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    draw.projection = scene->projection;
    draw.material = material;
    const native_render::MeshResourceView mesh{draw.mesh.resource, draw.mesh.revision, vertices};
    const native_render::DecodedImageView image{decoded.texture.resource,
                                                decoded.texture.revision,
                                                decoded.texture.width,
                                                decoded.texture.height,
                                                decoded.rgba8,
                                                decoded.mipLevels};
    if (!native_render::submit_model(draw, mesh,
                                     std::span<const native_render::DecodedImageView>(&image, 1))) {
        ++g_stats.rejected;
        return false;
    }
    ++g_stats.submitted;
    return true;
}

void report_semantic_particle_stats() noexcept {
    lucent::info("semantic",
                 "native JPA billboards: submitted={} rejected={} invisible={} "
                 "unsupported-shape={} (type={} program={}) invalid-texture={} "
                 "decode-failures={} no-scene={} view-failures={}",
                 g_stats.submitted, g_stats.rejected, g_stats.invisible, g_stats.unsupportedShape,
                 g_stats.unsupportedType, g_stats.unsupportedProgram, g_stats.invalidTexture,
                 g_stats.decodeFailures, g_stats.noScene, g_stats.viewFailures);
}

} // namespace sb::recomp
