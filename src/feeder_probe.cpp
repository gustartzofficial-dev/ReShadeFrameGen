#include "feeder_probe.hpp"

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
    return s;
}
}
