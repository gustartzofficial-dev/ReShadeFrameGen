#pragma once

#include <reshade.hpp>

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
};

void resolve(reshade::api::effect_runtime *runtime);
void clear(reshade::api::effect_runtime *runtime);
Snapshot snapshot();
}
