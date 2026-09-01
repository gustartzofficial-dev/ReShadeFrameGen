# ReShadeFrameGen — clean-slate DLSS-G host

This is a **from-scratch replacement** for the old optical-flow / extra-`Present()` experiment.

The old architecture is intentionally not compiled or referenced. This project does **not** make a synthetic frame and call `Present()` itself. On the NVIDIA path it attaches to an already initialized NVIDIA Streamline host and controls **real DLSS Frame Generation** through `slDLSSGSetOptions` / `slDLSSGGetState`.

## Why the rewrite

DLSS Neural Rendering and DLSS Frame Generation have different integration models:

- DLSS5-Feeder can manufacture a normal NGX/DLSS evaluate. RenoDX's NR hook sees that evaluate and inserts Neural Rendering.
- DLSS-G is presentation infrastructure. Streamline owns the FG swapchain/pacer and consumes depth, motion vectors, common constants, Reflex markers and the real presented color.

Trying to reproduce that with extra ReShade `Present()` calls is what made the old project halve/replace the game's existing cadence instead of adding frames.

## Version 0.1 goal

This first clean build deliberately does one thing well:

1. detect an **already initialized** Streamline/RenoDX DLSS host;
2. resolve the real NVIDIA DLSS-G feature functions from `sl.interposer.dll`;
3. call `slDLSSGSetOptions` on ReShade's presentation callback;
4. query `slDLSSGGetState` and expose the real NVIDIA runtime errors/counters.

There is no software FG fallback in 0.1. If the real host is absent, the checkbox stays disabled.

## 64-bit D3D11 test stack

For D3D11, this build expects the **current ShortFuse `renodx-dlss.addon64`**, because current DLSS5-Feeder documentation says it now owns D3D9/D3D11/D3D12 presentation through a same-adapter D3D12 endpoint and shares real depth/MV for D3D11.

Do not confuse it with the older `renodx-dlss5.addon64`, which is primarily the Neural Rendering/NGX hook used by classic DLSS5-Feeder setups.

Recommended files beside the game EXE:

```text
game.exe
dxgi.dll                     ReShade 6.8+ add-on build
ReShade.ini
ReShadeFrameGen.addon64      this project
renodx-dlss.addon64          current ShortFuse host
sl.interposer.dll
sl.common.dll
sl.dlss_g.dll
sl.reflex.dll
nvngx_dlssg.dll
...the rest of the matching Streamline package...
```

The current RenoDX host needs to be loaded **early**. Follow the RenoDX build's current install instructions; if it requires `LoadFromDllMain`, use that. Do not have two different tools fighting over the same Streamline/NGX hooks.

### About DLSS5-Feeder

For a 64-bit D3D11/D3D12 game, do **not** load `dlss5-feed.addon64` alongside the newer `renodx-dlss.addon64` unless the RenoDX/Feeder authors specifically say that combination is supported. The Feeder README currently says the newer RenoDX addon supersedes Feeder for those paths.

`DLSS5_Feed.fx` is still useful as a diagnostic/reference guide contract. Version 0.1 detects its `DLSS5_MV` and `DLSS5_Depth` textures but deliberately does not retag them into Streamline yet: the early host owns the authoritative frame token, common constants, resource lifetime and Reflex markers.

## Testing

1. Build and copy `ReShadeFrameGen.addon64` beside the game EXE.
2. Install ReShade 6.8+ with full add-on support.
3. Install the current `renodx-dlss.addon64` + matching Streamline/NVIDIA runtime files and ensure the RenoDX host loads early.
4. Launch a 64-bit D3D11 or D3D12 game.
5. Press Home -> **Add-ons -> DLSS-G Host**.
6. All required host lines should turn green.
7. Enable **real NVIDIA DLSS Frame Generation** and select x2 first.

See `TEST_FIRST_RUN.md` for the exact interpretation of each failure state.

A successful test should show:

```text
DLSS-G runtime status: OK
SetOptions: OK
GetState: OK
App Present callbacks: ~60 fps
DLSS-G frames actually presented: ~120 fps   (x2 example)
```

The DLSS-G output counter comes from NVIDIA's `DLSSGState::numFramesActuallyPresented`, not from our own fake counter.

## If it does not enable

The overlay decodes NVIDIA's own failure bits. The important ones are:

- **Reflex missing** — the host is not supplying the Streamline Reflex cadence DLSS-G requires.
- **Common constants invalid** — camera/frame constants are absent or wrong.
- **GetCurrentBackBufferIndex not called** — the Streamline swapchain contract is incomplete.
- **Feature not loaded / missing hooks** — Streamline was not initialized early enough with DLSS-G.

That is exactly why 0.1 fails closed instead of silently falling back to the old interpolation code.

## What comes next

Once 0.1 proves we can reliably activate/query DLSS-G through the existing early host, phase 2 is the Feeder-style resource handoff:

- acquire `DLSS5_MV` / `DLSS5_Depth` (or the newer RenoDX guide resources);
- hook into the host's **existing** frame token instead of creating a competing token;
- attach the guides with `slSetTagForFrame(... eValidUntilPresent ...)`;
- provide/override common constants only where the host leaves a gap;
- add HUD-less/UI tagging;
- add a generic D3D11 profile path.

The invariant is: **Streamline owns presentation. This addon never calls an extra swapchain Present to generate FPS.**

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output:

```text
build/addon/Release/ReShadeFrameGen.addon64
```

(or under `build/addon/` depending on the generator).

## Dependencies and licensing

This repository only contains our source. It fetches headers from ReShade 6.8.0 and NVIDIA Streamline 2.12.0 at build time. It does **not** redistribute RenoDX binaries, NVIDIA NGX binaries or Streamline runtime DLLs.

## How to interpret the first test

The most valuable v0.1 result is whether the **DLSS-G feature itself is already loaded** by the early RenoDX/Streamline host.

### Case A — all green

If `sl.interposer.dll`, `sl.dlss_g.dll`, `renodx-dlss.addon64` and `DLSS-G feature loaded by Streamline` are all green, v0.1 can call NVIDIA's real `slDLSSGSetOptions`/`slDLSSGGetState`. Any remaining red runtime status is then a concrete resource/Reflex/common-constants integration gap.

### Case B — Streamline + RenoDX are loaded, but `DLSS-G feature loaded` is red

That means the host initialized Streamline without requesting `kFeatureDLSS_G` (or the plugin failed to load). **Do not try to call `slInit` a second time from a late ReShade callback.** The next implementation step is an early bootstrap/interposer hook that amends the host's initial Streamline feature list before device/swapchain creation.

This is intentional: the rewrite is designed to expose the first missing real DLSS-G contract instead of hiding it behind home-made frame insertion.
