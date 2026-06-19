# `shaders/` — GLSL → SPIR-V

The GLSL 4.50 shaders, compiled to SPIR-V at build time (`add_shader` in
`CMakeLists.txt` — uses a system `glslc`/`glslangValidator`, else a glslang built
from source). Output: `build/bin/shaders/<name>.spv`.

> Part of the [**Code Index**](../docs/CODE_INDEX.md) (see [Shaders](../docs/CODE_INDEX.md#shaders)).
> Per-shader bindings/entry points are indexed in
> [`docs/index/SYMBOLS.md`](../docs/index/SYMBOLS.md).

## Shared descriptor set 0

`binding 0` CameraUBO · `binding 1` block `sampler2DArray` · `binding 2` entity
skin `sampler2DArray` · `binding 3` light volume `sampler3D` (S7).

## Shaders

| File | Stage | Purpose |
|---|---|---|
| `chunk.vert` / `chunk.frag` | vert / frag | Chunk geometry: world transform + foliage sway; block texture, sky+block light (per-pixel from the light volume), directional sun/moon + held point light, biome tint, retro dither/palette. |
| `chunk_indirect.vert` | vert | GPU-driven variant: per-chunk translation read from an SSBO by `gl_InstanceIndex`. |
| `chunk_cull.comp` | comp | Frustum-culls resident chunk slots → `VkDrawIndexedIndirectCommand`s (local size 64). |
| `sky.vert` / `sky.frag` | vert / frag | Fullscreen triangle: analytic Rayleigh+Mie scatter, sun/moon discs, stars/Milky-Way/planets, sunset bands, volumetric clouds. |
| `entity.vert` / `entity.frag` | vert / frag | Rig-baked entity geometry; block- or skin-atlas with alpha cutout + directional light. |
| `composite.vert` / `composite.frag` | vert / frag | Upscale the low-res scene + ordered dither + retro colour-bits/interlace. |
| `ui.vert` / `ui.frag` | vert / frag | 2D overlay: pixel→clip; font atlas (R = coverage) or block-icon sampling. |

## Notes

- Reversed-Z: the scene clears depth to 0 (far) and near maps to 1.
- The chunk vertex layout is defined in `src/render/Vertex.h` and **must** stay in
  sync with `chunk.vert`'s inputs and `Vertex::attributeDescriptions()`.
- GPU-driven culling (`chunk_cull.comp` + `chunk_indirect.vert`) is opt-in via the
  `VG_GPUCULL` env var; see [`docs/GPU_DRIVEN_RENDERING.md`](../docs/GPU_DRIVEN_RENDERING.md).
