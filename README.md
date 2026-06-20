# ReShade DX11 FrameGen Add-on

Experimental ReShade add-on that ports the DX11 frame generation research path from `fsr-injector` into a ReShade add-on.

## Current status

This is a first side-project foundation, not a production FSR3 replacement yet.

It currently does:

- DX11-only runtime detection through the ReShade add-on API.
- Captures previous/current backbuffer frames.
- Runs the same pyramid/coarse-to-fine optical-flow idea from the injector.
- Smooths the flow field with a confidence channel.
- Writes a motion-interpolated frame back to the current backbuffer.
- Exposes basic settings in the ReShade add-on overlay.

Important limitation: this version does **not** safely insert an extra `IDXGISwapChain::Present` yet. ReShade add-ons do not provide the same original-present trampoline that our proxy injector owns. Calling the native swapchain `Present` from inside ReShade callbacks can recurse or conflict with ReShade, so this first build is a safe motion-interpolation/framegen-preview path. The next step is an optional generated-present experiment guarded behind a setting once the base path is stable.

## Requirements

- ReShade with full add-on support enabled.
- DX11 game.
- Windows x64.

ReShade add-ons are DLLs that use ReShade's header-only add-on API; after building, rename/copy the DLL to `.addon` and place it in the add-on search directory. ReShade documents this flow in its add-on API reference.

## Building on GitHub

1. Create a new GitHub repo.
2. Upload this project to the repo root.
3. Open **Actions**.
4. Run **Build ReShade DX11 FrameGen Addon**.
5. Download the artifact.
6. Put `ReShadeDX11FrameGen.addon` next to ReShade in the game folder.

## Controls / settings

Open the ReShade overlay and look for the **DX11 FrameGen** add-on panel.

Settings:

- Enable framegen preview
- Use pyramid optical flow
- Smooth flow
- HUD/static-region protection
- Strength
- Debug overlay

## Suggested test process

1. Start with all normal ReShade effects disabled.
2. Enable this add-on only.
3. Test at 30 FPS and 60 FPS.
4. Watch for ghosting, HUD smear, flicker, and input-latency feel.
5. If unstable, disable the add-on from the ReShade overlay and restart.

## Roadmap

- Add safe generated-present experiment.
- Add proper frame pacing controls.
- Add depth-buffer integration using ReShade's existing depth selection when available.
- Add motion confidence visualization.
- Add DX9/DX10 compatibility only after DX11 is stable.
