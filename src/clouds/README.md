# `src/clouds/` — volumetric clouds

The volumetric cloud system: the 3D noise textures clouds are raymarched
against, the spatial weather field, and the weather/cloud evolution that feeds
parameters to the sky shader.

> Part of the [**Code Index**](../../docs/CODE_INDEX.md) (*clouds/* map).
> Exhaustive symbols: [`docs/index/SYMBOLS.md`](../../docs/index/SYMBOLS.md).

## Files

| File | Responsibility | Key symbols |
|---|---|---|
| `CloudNoise.{h,cpp}` | Base (Perlin-Worley, 64³) + detail (Worley fBm, 32³) 3D noise textures; generated on CPU and cached to disk. | `CloudNoise` |
| `WeatherMap.{h,cpp}` | Static 64² spatial weather field (coverage/type offsets), wind-scrolled in-shader. | `WeatherMap` |
| `CloudSystem.{h,cpp}` | Weather evolution (diurnal + multi-day drift) → `GpuParams` (13 vec4s) appended to the sky UBO. | `CloudSystem::update`, `gpuParams()`, `struct GpuParams` |

## Notes & cross-refs

- `CloudSystem::update(dt, dayNight)` advances wind/coverage/type; `gpuParams()`
  packs everything into a std140 vec4-only block that `SkyRenderer` uploads with
  the rest of the sky state. The actual raymarch lives in `shaders/sky.frag`.
- Cloud look is data-driven from `assets/clouds.yaml` (see
  [Data & asset catalog](../../docs/CODE_INDEX.md#data--asset-catalog)).
- `CloudNoise` generation is slow in Debug; the disk cache avoids regenerating
  every launch.

## Read first

`CloudSystem.h` (the `GpuParams` contract) → `CloudNoise.h`.
