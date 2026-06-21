# ReShade DX11 FrameGen Add-on

Experimental ReShade add-on for DX11 optical-flow frame generation experiments.

## Current status

This build is an active diagnostic/preview build. It is intended to prove that the add-on can actually render an interpolated frame into the ReShade backbuffer, not just show a menu.

## Controls

Open the ReShade overlay and use the add-on panel:

- Enable framegen: turns the pipeline on.
- Active preview render: draws the interpolated preview into the current backbuffer.
- Experimental extra Present: attempts an additional swapchain Present after drawing the generated frame.
- Extra Present interval: limits extra presents for safety.
- Flow downscale: higher is faster and lower quality.
- Preview every N frames: throttles preview rendering.
- Strength: interpolation strength.

## Debug fields

The overlay shows:

- Status
- Last HR
- Real frames
- Interpolated frames
- Extra presents
- Draw attempts/success
- Flow grid

If Interpolated frames stays at 0, read Status and Last HR. That tells which stage failed.

## Safe test settings

Start with:

- Enable framegen: ON
- Active preview render: ON
- Experimental extra Present: OFF
- Fast mode: ON
- Fast search: ON
- Smooth flow: OFF
- Flow downscale: 24 or 32
- Preview every N frames: 2

Only enable Experimental extra Present after preview draw attempts and success increase.
