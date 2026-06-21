# ReShade DX11 FrameGen Add-on

Experimental ReShade add-on that ports the DX11 frame generation research path from `fsr-injector` into a ReShade add-on.

## Current status

This is a first side-project foundation, not a production FSR3 replacement yet. This build adds a safer performance-first preview path because full optical flow inside ReShade can be very expensive.

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
- Fast mode
- Fast search
- Flow downscale
- Preview every N frames
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


## Compatibility note

This project pins the ReShade SDK headers to `v6.7.3` so the built add-on requests ReShade add-on API 18, matching ReShade 6.7.3. Building against ReShade `main` can produce an add-on that requests a newer API and fails to load with `requested API version ... is not supported`.


## Performance notes

If enabling preview tanks FPS, start with:

- Fast mode: on
- Fast search: on
- Smooth flow: off
- Flow downscale: 24 or 32
- Preview every N frames: 2 or 3

This add-on currently runs optical-flow preview inside ReShade, so it is not expected to behave like true extra-present FSR3 yet. The immediate goal is a fast stable motion-preview path before experimenting with generated-present insertion.

## Experimental generated-frame presentation

This build adds an **Experimental extra Present** toggle. Preview mode still renders the interpolated image into the current backbuffer. Extra Present mode attempts a true generated-frame presentation:

1. Copy the current real frame into private history.
2. Generate the interpolated frame from previous/current history.
3. Present that generated frame immediately.
4. Restore the real current frame to the next swapchain buffer so the normal game/ReShade present can continue.

This is intentionally risky and disabled by default. If it stutters, flickers, or crashes in a game, turn it off and use preview mode while we tune the pacing path.
