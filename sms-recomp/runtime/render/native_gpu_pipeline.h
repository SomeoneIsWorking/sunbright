#pragma once

#include "native_render.h"

#include <SDL3/SDL_gpu.h>

inline constexpr SDL_GPUTextureFormat kNativeColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
inline constexpr SDL_GPUTextureFormat kNativeDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

bool sbr_native_gpu_pipeline_init(SDL_GPUDevice* device);
void sbr_native_gpu_pipeline_shutdown() noexcept;
uint32_t sbr_native_gpu_pipeline_key(SbrDepthState state);
SDL_GPUGraphicsPipeline* sbr_native_gpu_pipeline_for(SbrDepthState state);
