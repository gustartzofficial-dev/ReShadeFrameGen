#include "feeder_probe.hpp"

#include <d3d11.h>

namespace fg::guides
{
namespace
{
reshade::api::effect_runtime *g_runtime = nullptr;
reshade::api::effect_technique g_technique{};
reshade::api::effect_texture_variable g_mv{};
reshade::api::effect_texture_variable g_depth{};
reshade::api::effect_texture_variable g_color{};
reshade::api::effect_texture_variable g_mask{};

ID3D11Texture2D *native_texture(reshade::api::effect_runtime *runtime, reshade::api::effect_texture_variable variable)
{
    if (runtime == nullptr || variable.handle == 0 || runtime->get_device() == nullptr ||
        runtime->get_device()->get_api() != reshade::api::device_api::d3d11)
        return nullptr;

    reshade::api::resource_view srv{};
    reshade::api::resource_view srv_srgb{};
    runtime->get_texture_binding(variable, &srv, &srv_srgb);
    if (srv.handle == 0)
        srv = srv_srgb;
    if (srv.handle == 0)
        return nullptr;

    const reshade::api::resource resource = runtime->get_device()->get_resource_from_view(srv);
    if (resource.handle == 0)
        return nullptr;

    // ReShade's D3D11 resource handle is the native ID3D11Resource pointer. Texture variables in
    // DLSS5_Feed.fx are Texture2D resources, so the object is an ID3D11Texture2D. We deliberately
    // return a borrowed pointer and never mutate/ref-count the effect-owned object here.
    return reinterpret_cast<ID3D11Texture2D *>(static_cast<uintptr_t>(resource.handle));
}
}

void resolve(reshade::api::effect_runtime *runtime)
{
    if (runtime == nullptr)
        return;

    g_runtime = runtime;
    g_technique = runtime->find_technique("DLSS5_Feed.fx", "DLSS5_Feed");
    g_mv    = runtime->find_texture_variable("DLSS5_Feed.fx", "DLSS5_MV");
    g_depth = runtime->find_texture_variable("DLSS5_Feed.fx", "DLSS5_Depth");
    g_color = runtime->find_texture_variable("DLSS5_Feed.fx", "DLSS5_ColorInput");
    g_mask  = runtime->find_texture_variable("DLSS5_Feed.fx", "DLSS5_Mask");
}

void clear(reshade::api::effect_runtime *runtime)
{
    if (runtime != nullptr && runtime != g_runtime)
        return;
    g_runtime = nullptr;
    g_technique = {};
    g_mv = {};
    g_depth = {};
    g_color = {};
    g_mask = {};
}

Snapshot snapshot()
{
    Snapshot s{};
    s.effect_present = g_technique.handle != 0;
    s.effect_enabled = g_runtime != nullptr && g_technique.handle != 0 && g_runtime->get_technique_state(g_technique);
    s.motion_vectors = g_mv.handle != 0;
    s.depth = g_depth.handle != 0;
    s.color = g_color.handle != 0;
    s.mask = g_mask.handle != 0;

    if (g_runtime != nullptr && g_runtime->get_device() != nullptr &&
        g_runtime->get_device()->get_api() == reshade::api::device_api::d3d11)
    {
        s.native_mv_ready = native_texture(g_runtime, g_mv) != nullptr;
        s.native_depth_ready = native_texture(g_runtime, g_depth) != nullptr;
    }
    return s;
}

bool acquire_native_textures(reshade::api::effect_runtime *runtime, NativeTextures &out)
{
    out = {};
    if (runtime == nullptr || runtime != g_runtime || runtime->get_device() == nullptr ||
        runtime->get_device()->get_api() != reshade::api::device_api::d3d11)
        return false;

    out.motion_vectors = native_texture(runtime, g_mv);
    out.depth = native_texture(runtime, g_depth);
    out.color = native_texture(runtime, g_color);
    out.mask = native_texture(runtime, g_mask);
    return out.motion_vectors != nullptr && out.depth != nullptr;
}
}
