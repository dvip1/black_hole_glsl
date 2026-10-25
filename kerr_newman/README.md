# TON 618 · Kerr–Newman

A second take on TON 618, rendered as a **rotating, charged** black hole using
the Kerr–Newman metric. It uses the same native Wayland + EGL + OpenGL 3.3 host
as the Schwarzschild version in the parent directory, and does not use X11 or XWayland.

## Build & run

```sh
make
./ton618-kn                  # a = 0.9, Q = 0.3
./ton618-kn -a 0.99 -q 0     # near-extremal Kerr
./ton618-kn -a -0.9          # retrograde disk (ISCO pushed out to ~8.7 M)
./ton618-kn -a 0 -q 0.95     # Reissner–Nordström (charged, non-rotating)
./ton618-kn -s 1.0 -f        # full resolution, fullscreen
```

`make run` checks for `nvidia-smi`. If it finds it, it runs on the NVIDIA GPU
using PRIME render offload; otherwise it uses the default GPU. You can force
the choice with `make run NVIDIA=1` or `NVIDIA=0`, and pass flags with
`make run ARGS="-f"`.

The default internal render scale is 0.6, because full Kerr–Newman geodesics
cost much more than the Schwarzschild shortcut. Raise it with `-s` if your GPU
can handle it.

## Controls

| Input       | Action                                  |
|-------------|-----------------------------------------|
| Left drag   | Orbit camera                            |
| Right drag  | Move window                             |
| Scroll      | Zoom                                    |
| 1 / 2       | Spin a − / +  (negative = retrograde)   |
| 3 / 4       | Charge Q − / +                          |
| J           | Toggle relativistic jets                |
| E           | Toggle ergosphere glow                  |
| F / F11     | Fullscreen                              |
| Space       | Pause                                   |
| R           | Hot-reload `shaders/`                   |
| Esc / Q     | Quit                                    |

a² + Q² is kept below 1, so there is always a horizon and never a naked singularity.

## Physics

- **Metric:** Kerr–Newman in *outgoing* Kerr–Schild Cartesian coordinates,
  `g = η + f k⊗k` with `f = r²(2Mr − Q²)/(r⁴ + a²z²)`. The inverse metric is
  exact, and the spin axis has no coordinate singularity.
- **Geodesics:** Hamiltonian `H = ½ g^{μν} p_μ p_ν`, integrated with RK4.
  Rays are traced backwards in time. The outgoing chart is used because shadow
  rays, followed back in time, cross the *past* horizon, which only the outgoing
  chart covers regularly.
- **Horizon:** `r+ = M + √(M² − a² − Q²)`.
- **Ergosphere:** `r_E(θ) = M + √(M² − Q² − a² cos²θ)`, drawn as a glow on rays
  that skim it and escape. Every captured ray crosses it, so it isn't drawn on those.
- **Disk:** starts at the Kerr–Newman ISCO, found on the CPU by minimising
  the circular-orbit energy. Gas moves with Keplerian angular velocity
  `Ω = √(Mr − Q²)/(r² + a√(Mr − Q²))`, and the redshift `g = E_obs/E_emit` is
  computed from the emitter's 4-velocity, so Doppler beaming, gravitational
  redshift and frame dragging come out together.
- **Visible effects of spin:** the shadow is D-shaped and shifted, flattened on
  the side where the disk approaches you, and the inner disk gets closer and
  hotter as a → 1.
- **Jets:** bipolar jets along the spin axis (Blandford–Znajek-style, artistic),
  Doppler-boosted with β = 0.6.

Astrophysical black holes are expected to be nearly neutral, so the charge
here is for exploration, not a claim about TON 618.
