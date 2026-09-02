#pragma once

#include <reshade.hpp>

struct ID3D11Texture2D;

namespace fg::guides
{
struct Snapshot
{
    bool effect_present = false;
    bool effect_enabled = false;
    bool motion_vectors = false;
    bool depth = false;
    bool color = false;
    bool mask = false;
    bool native_mv_ready = false;
    bool native_depth_ready = false;
};

struct NativeTextures
{
    // Borrowed pointers. They remain owned by ReShade/the effect runtime and are only valid for
    // the duration of the current callback. Do not Release() them.
    ID3D11Texture2D *motion_vectors = nullptr;
    ID3D11Texture2D *depth = nullptr;
    ID3D11Texture2D *color = nullptr;
    ID3D11Texture2D *mask = nullptr;
};

void resolve(reshade::api::effect_runtime *runtime);
void clear(reshade::api::effect_runtime *runtime);
Snapshot snapshot();

// Resolve the actual D3D11 textures bound to DLSS5_Feed.fx this frame. This is the point where
// v0.3 stops being a detector and starts consuming Feeder output as the DLSS-G guide provider.
bool acquire_native_textures(reshade::api::effect_runtime *runtime, NativeTextures &out);
}
