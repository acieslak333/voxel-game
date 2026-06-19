# `src/player/` — controller, camera, inventory, crafting

The player: the movement/collision controller, the camera, and the inventory /
crafting / chest / equipment / save systems. The data systems here are pure
logic and are exercised by `--logictest` (round-trip and physics assertions).

> Part of the [**Code Index**](../../docs/CODE_INDEX.md) (*player/* map).
> Exhaustive symbols: [`docs/index/SYMBOLS.md`](../../docs/index/SYMBOLS.md).

## Files

| File | Responsibility | Key symbols |
|---|---|---|
| `PlayerController.{h,cpp}` | Walking (gravity, AABB collision, swim/drown) + free-fly; health + equipment modifiers. | `update(dt, input)`, `fallDamage`, `SolidFn`/`BoxesFn`/`WaterFn` |
| `Camera.{h,cpp}` | Yaw/pitch camera; look deltas and the view matrix. | `addLook`, `front`, `viewMatrix` |
| `Inventory.{h,cpp}` | 9-slot hotbar + backpack; stack merge/add/remove. | `add`, `remove`, `takeFromSelected`, `count` |
| `Crafting.{h,cpp}` | Recipe loading (`recipes.yaml`) + craftability; pure logic. | `craftable`, `craft` |
| `ChestStore.{h,cpp}` | Per-position chest storage with a packed integer key. | `serialize`, `deserialize` |
| `Equipment.h` | 1 armour + 4 trinket slots → aggregated `Stats`. | `computeStats` |
| `PlayerSave.h` | `PlayerSave` struct + versioned binary (de)serialize (round-trip tested). | `serialize`, `deserialize` |

## Notes & cross-refs

- `PlayerController::update` takes caller-supplied collision predicates
  (`SolidFn`/`BoxesFn`/`WaterFn`) so it has no direct `World` dependency — that is
  what keeps it unit-testable. `App::applyEquipmentStats` pushes armour/trinket
  bonuses into the controller each frame.
- Persistence: `App::savePlayer`/`loadPlayer` write `<save dir>/player.dat`;
  chests go to `<save dir>/chests.dat`.

## Read first

`PlayerController.h` (the physics interface + predicates) → `Inventory.h` →
`PlayerSave.h`.
