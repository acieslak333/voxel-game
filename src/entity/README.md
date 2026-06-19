# `src/entity/` — models, rigs, mobs, items, particles

Everything that isn't terrain: the Blockbench model loader, the pure-CPU
box-part armature that animates and bakes meshes, passive critters, dropped
items, and particle effects. All of these are **pure simulations** (no Vulkan) so
they stay headless-testable via `--logictest`.

> Part of the [**Code Index**](../../docs/CODE_INDEX.md) (*entity/* map).
> Exhaustive symbols: [`docs/index/SYMBOLS.md`](../../docs/index/SYMBOLS.md).

## Files

| File | Responsibility | Key symbols |
|---|---|---|
| `Armature.{h,cpp}` | Box-part rig: skeleton + animation clips → baked world-space mesh. Pure math. | `restPose`, `sampleClip`, `worldMatrices`, `bakeMesh`, `struct Skeleton`/`AnimationClip` |
| `BlockbenchModel.{h,cpp}` | Loads `.bbmodel` JSON → `Skeleton` + skin (embedded or referenced PNG). | `loadBlockbenchModel(path)` |
| `Critters.{h,cpp}` | Passive-mob AI (wander/turn/gravity) via a `solid()` predicate. | `Critters::update`, `struct Critter` |
| `ItemEntity.{h,cpp}` | Dropped-item physics + magnet pickup into the inventory. | `ItemEntities::spawn`/`update`, `struct ItemEntity` |
| `Particles.{h,cpp}` | Data-driven particle bursts (`.prtcl`); gravity/drag/aging. | `Particles::spawnEffect`/`update`, `struct ParticleEffect` |

## Pipeline & cross-refs

Blockbench JSON → `loadBlockbenchModel` → `Skeleton` → `App::buildModels`
(discovers `assets/models/<name>/<name>.bbmodel`, builds the skin atlas) →
`Armature::sampleClip`/`worldMatrices`/`bakeMesh` → `EntityRenderer` draws the
baked vertices. Tool/held models and the critter rig are *generated* by
`tools/gen_tool_models.py` and `tools/gen_critter_model.py`. Particle effects are
tuned in `tools/particle_tool.py`.

## Read first

`Armature.h` (the rig data + the pure functions) → `BlockbenchModel.h`.
