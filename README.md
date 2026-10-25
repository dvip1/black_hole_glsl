# TON 618

A real-time GLSL black hole (TON 618, ~66 billion M☉) rendered with native
Wayland + EGL + OpenGL 3.3 core. Written in C, with no X11 or XWayland.

## Build & run

```sh
make
./ton618            # windowed
./ton618 -f         # fullscreen
./ton618 -s 0.5     # render at half resolution (faster on weak GPUs)
```

`make run` checks for `nvidia-smi`. If it finds it, it runs on the NVIDIA GPU
using PRIME render offload; otherwise it uses the default GPU. You can force
the choice with `make run NVIDIA=1` or `NVIDIA=0`, and pass flags with
`make run ARGS="-f"`.

Dependencies: `wayland-devel`, `mesa-libEGL-devel`, `libglvnd-devel`
(`wayland-scanner`, `wayland-egl`, `wayland-cursor`). The xdg-shell and
xdg-decoration protocol XMLs are vendored in `protocol/`.

## Controls

| Input        | Action                         |
|--------------|--------------------------------|
| Left drag    | Orbit camera                   |
| Right drag   | Move window                    |
| Scroll       | Zoom                           |
| F / F11      | Toggle fullscreen              |
| Space        | Pause                          |
| R            | Hot-reload `shaders/` from disk |
| Esc / Q      | Quit                           |

## What the shader does

- Integrates photon geodesics in Schwarzschild spacetime (r_s = 1), giving the
  shadow, the photon ring, and the back of the disk lensed over and under the hole.
- Thin accretion disk from the ISCO (3 r_s) outward, with a Novikov–Thorne-like
  temperature profile and blackbody colour.
- Relativistic Doppler beaming plus gravitational redshift (g⁴ intensity), so the
  approaching side is brighter and bluer.
- Turbulence sheared by Keplerian rotation, plus volumetric haze near the disk
  plane and a lensed starfield.
