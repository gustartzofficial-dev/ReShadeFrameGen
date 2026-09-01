# Architecture

## Old project (retired)

```text
Game Present callback
  -> capture current/previous
  -> home-made optical flow
  -> draw synthetic image into game backbuffer
  -> call extra Present / wait
  -> return to game Present
```

That path cannot behave like a true frame-generation swapchain. Its pacing work is inside the producer's presentation path, so it steals or replaces the producer's display slots.

## New NVIDIA path

```text
D3D11/D3D12 game
        |
        v
ReShade + early RenoDX/Streamline host
        |
        +---- real game color/depth/MV/common constants/Reflex ---> Streamline
        |
        +---- ReShadeFrameGen 0.1 -------------------------------+
        |      only SetOptions/GetState on present thread        |
        v                                                        v
                 NVIDIA DLSS-G presentation path
                    real + generated frames
```

### Ownership rules

- **Game/RenoDX/Streamline host owns:** swapchain interception, D3D12 endpoint, resource tags, frame token, common constants, Reflex/PCL markers, UI recomposition resources, presentation pacing.
- **ReShadeFrameGen 0.1 owns:** user intent (on/off, multiplier), dependency diagnostics, DLSS-G status, output telemetry.
- **DLSS5_Feed contract:** probed but not claimed in 0.1. Phase 2 will only retag it after we can share the host's real frame token safely.

This prevents two integrations from independently calling `slGetNewFrameToken`, tagging the same viewport with mismatched frame indices, or both trying to own Present.
