#include <sunbright/native_render/particle_billboard.h>
#include <sunbright/native_render/semantic_sink.h>

#include "host_allocation_scope.h"
#include "native_j3d_scene.h"
#include "native_jut_texture_adapter.h"

#include <JSystem/JParticle/JPABaseShape.hpp>
#include <JSystem/JParticle/JPADraw.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <JSystem/JParticle/JPAParticle.hpp>
#include <JSystem/JParticle/JPAResourceManager.hpp>
#include <dolphin/gx/GXEnum.h>
#include <sb_log.h>

#include <array>
#include <cstdint>
#include <span>

namespace {

std::uint64_t g_submitted = 0;
std::uint64_t g_rejected = 0;

bool direct_texture_program(JPABaseShape& shape) noexcept {
    return shape.unk48.unk0 == GX_CC_ZERO && shape.unk48.unk4 == GX_CC_TEXC &&
           shape.unk48.unk8 == GX_CC_ONE && shape.unk48.unkC == GX_CC_ZERO;
}

bool build_raster_policy(JPABaseShape& shape,
                         sb::native_render::ModelRasterPolicy& policy) noexcept {
    using namespace sb::native_render;
    ModelRasterPolicy result{};
    if (shape.getZCmpFunction() != GX_LEQUAL && shape.isEnableZCmp())
        return false;
    result.depthTest = shape.isEnableZCmp();
    result.depthCompare =
        shape.isEnableZCmp() ? ModelDepthCompare::LessOrEqual : ModelDepthCompare::Always;
    result.depthWrite = shape.isEnableZCmpUpdate();

    const GXCompare compare0 = shape.getAlphaCmpComp0();
    const GXCompare compare1 = shape.getAlphaCmpComp1();
    const GXAlphaOp operation = shape.getAlphaCmpOp();
    if (compare0 == GX_ALWAYS && compare1 == GX_ALWAYS &&
        (operation == GX_AOP_AND || operation == GX_AOP_OR || operation == GX_AOP_XNOR)) {
        result.alphaTest = ModelAlphaTest::PassAll;
    } else if (compare0 == GX_GEQUAL && shape.getAlphaCmpRef0() == 0x80 && compare1 == GX_LEQUAL &&
               shape.getAlphaCmpRef1() == 0xFF && operation == GX_AOP_AND) {
        result.alphaTest = ModelAlphaTest::GreaterOrEqualHalf;
    } else {
        return false;
    }

    switch (shape.getBlendMode1()) {
    case GX_BM_NONE:
        if (shape.getSrcBlendFactor1() != GX_BL_ONE || shape.getDstBlendFactor1() != GX_BL_ZERO)
            return false;
        result.blend = ModelBlendMode::Replace;
        break;
    case GX_BM_BLEND:
        if (shape.getSrcBlendFactor1() == GX_BL_SRCALPHA &&
            shape.getDstBlendFactor1() == GX_BL_INVSRCALPHA) {
            result.blend = ModelBlendMode::SourceAlpha;
        } else if (shape.getSrcBlendFactor1() == GX_BL_SRCALPHA &&
                   shape.getDstBlendFactor1() == GX_BL_ONE) {
            result.depthTest = false;
            result.depthWrite = false;
            result.depthCompare = ModelDepthCompare::Always;
            result.blend = ModelBlendMode::Additive;
        } else {
            return false;
        }
        break;
    default:
        return false;
    }
    policy = result;
    return true;
}

bool view_point(const float (*view)[4], const JGeometry::TVec3<f32>& world,
                sb::native_render::Vec3& eye) noexcept {
    eye = {view[0][0] * world.x + view[0][1] * world.y + view[0][2] * world.z + view[0][3],
           view[1][0] * world.x + view[1][1] * world.y + view[1][2] * world.z + view[1][3],
           view[2][0] * world.x + view[2][1] * world.y + view[2][2] * world.z + view[2][3]};
    return true;
}

} // namespace

extern "C" bool sb_native_particle_submit_billboard(const JPADrawContext* context,
                                                    JPABaseParticle* particle) {
    if (!sb::native_render::has_semantic_sink() || context == nullptr || particle == nullptr)
        return false;
    const auto* scene = sb::current_native_j3d_scene();
    if (scene == nullptr || context->mBaseShape == nullptr || context->mTexResource == nullptr ||
        context->pcb == nullptr || context->pcb->mViewMtx == nullptr ||
        context->mBaseShape->getType() != 2 || !direct_texture_program(*context->mBaseShape)) {
        ++g_rejected;
        return false;
    }
    if (particle->isInvisibleParticle())
        return false;

    const std::uint16_t textureIndex = particle->getDrawParamPPtr()->unk3A;
    if (context->mTexResource->pTexResArray == nullptr ||
        textureIndex >= context->mTexResource->registNum ||
        context->mTexResource->pTexResArray[textureIndex] == nullptr) {
        ++g_rejected;
        return false;
    }
    const sb::HostAllocationScope hostAllocations;
    sb::CapturedNativeTexture captured{};
    const char* textureError = "unknown texture error";
    if (!sb::capture_native_jut_texture(context->mTexResource->pTexResArray[textureIndex]->unk8,
                                        captured, textureError)) {
        ++g_rejected;
        return false;
    }

    sb::native_render::ModelRasterPolicy raster{};
    if (!build_raster_policy(*context->mBaseShape, raster)) {
        ++g_rejected;
        return false;
    }
    JGeometry::TVec3<f32> world{};
    particle->getGlobalPosition(world);
    sb::native_render::Vec3 eye{};
    view_point(context->pcb->mViewMtx, world, eye);
    const JPADrawParams* params = particle->getDrawParamPPtr();
    sb::native_render::ParticleBillboardInput input{
        .eyeCenter = eye,
        .halfExtent = {params->mScaleX * context->pcb->unk4.x,
                       params->mScaleY * context->pcb->unk4.y},
        .pivot = {context->pcb->unk4.x == 0.0F ? 0.0F : context->pcb->unkC.x / context->pcb->unk4.x,
                  context->pcb->unk4.y == 0.0F ? 0.0F
                                               : context->pcb->unkC.y / context->pcb->unk4.y},
        .uv = {{{context->pcb->mTexCoords[0].x, context->pcb->mTexCoords[0].y},
                {context->pcb->mTexCoords[1].x, context->pcb->mTexCoords[1].y},
                {context->pcb->mTexCoords[2].x, context->pcb->mTexCoords[2].y},
                {context->pcb->mTexCoords[3].x, context->pcb->mTexCoords[3].y}}},
    };
    const auto vertices = sb::native_render::make_particle_billboard_mesh(input);
    const sb::native_render::PictureTexture texture = captured.texture;
    const sb::native_render::UnlitTexturedMaterial material{
        .texture = texture, .usesVertexColor = false, .raster = raster};
    sb::native_render::ModelDraw draw{};
    draw.instance = reinterpret_cast<std::uintptr_t>(particle);
    draw.mesh = {reinterpret_cast<std::uintptr_t>(particle),
                 sb::native_render::mesh_revision(vertices),
                 static_cast<std::uint32_t>(vertices.size())};
    draw.pose.count = 1;
    draw.pose.modelViews[0].value = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                     0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    draw.projection = scene->projection;
    draw.material = material;
    const sb::native_render::MeshResourceView mesh{draw.mesh.resource, draw.mesh.revision,
                                                   vertices};
    const sb::native_render::DecodedImageView image = captured.image_view();
    if (!sb::native_render::submit_model(
            draw, mesh, std::span<const sb::native_render::DecodedImageView>(&image, 1))) {
        ++g_rejected;
        return false;
    }
    ++g_submitted;
    return true;
}

extern "C" void sb_native_particle_report_stats(void) {
    sb_logf("semantic", "native JPA billboards: submitted=%llu rejected=%llu",
            static_cast<unsigned long long>(g_submitted),
            static_cast<unsigned long long>(g_rejected));
}
