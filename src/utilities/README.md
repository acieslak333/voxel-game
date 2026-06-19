# `src/utilities/` — allocation, hashing, noise

Low-level, dependency-free building blocks shared across subsystems: a CPU
free-list allocator, the canonical worldgen hash/randomness, and the noise stack
(a thin deterministic adapter over vendored FastNoise plus the layering/masking
used by worldgen).

> Part of the [**Code Index**](../../docs/CODE_INDEX.md) (*utilities/* map).
> Exhaustive symbols: [`docs/index/SYMBOLS.md`](../../docs/index/SYMBOLS.md).

## Files

| File | Responsibility | Key symbols |
|---|---|---|
| `alloc/SpanAllocator.{h,cpp}` | CPU free-list sub-allocator (best-fit + coalescing) backing the GPU mesh arenas. Main-thread only. | `allocate`, `free`, `largestFreeBlock` |
| `hash/Hash.h` | **Canonical worldgen randomness.** Floor-correct div/mod + 2D/3D cell hashes. | `floordiv`, `floormod`, `hash01` |
| `noise/Noise.{h,cpp}` | Deterministic adapter over FastNoise: `perlin`/`fbm`/`fbmEroded`/`worley`. | `Noise` |
| `noise/NoiseStack.{h,cpp}` | Weighted multi-layer noise blend + redistribution/terrace; auto-clamps invisible octaves. | `addLayer`, `value`, `setRedistribution`, `setTerrace` |
| `noise/NoiseMask.{h,cpp}` | `NoiseStack` → threshold/width band → `Falloff` curve → `[0,1]` weight. | `weight`, `enum Falloff` |
| `noise/NoiseLoad.h` | YAML → `NoiseStack`/`NoiseMask`; Catmull-Rom falloff LUT. | `loadStack`, `loadMask`, `buildFalloffLut` |
| `noise/FastNoise.{h,cpp}` | ⚠️ **VENDORED** — Jordan Peck FastNoise (MIT). Not documented per-symbol. | (external) |

## Invariants (read before editing)

- **`hash/Hash.h` is the single definition of worldgen randomness.** Changing a
  constant here re-rolls every world. All per-cell random decisions (tree/feature
  gates, ore veins, variant selection) funnel through `hash01`. Window index math
  uses `floordiv`/`floormod` because truncating `/`/`%` are wrong for negatives.
- **All noise is a pure function of `(seed, coords)`** — identical on every
  thread, which is what makes pregen/streaming seam-safe. `NoiseStack::addLayer`
  clamps invisible octaves (the heavy-cost gotcha: editor-authored stacks with
  amplitude near world height make every cell pay the density path).
- `noise/FastNoise.*` is vendored and excluded from the Doxygen pass and the
  symbol index — do not edit it to add docs.

## Read first

`hash/Hash.h` → `noise/Noise.h` → `noise/NoiseStack.h`.
