# Code Review — 2026-06-13

## Theme: streaming & threading correctness, and the remaining frame-hitch debt

**Why this theme.** The engine now runs four things concurrently against one mutable
world: the main thread (game logic, edits, Vulkan), a mesh worker pool (greedy-meshes
chunks), an async relight task (floods the light fields after a window step), and the
new strip pregeneration task (generates the next window edge). The performance work
that landed this week (startup melt-in, background strip pregen) made the game fast
*because* of this concurrency — which also makes its invariants the most
expensive-to-debug part of the codebase if they rot. This review walks every seam
between those threads, plus the frame-hitch sources that survived the optimization
passes, and the conventions (config-as-data, the selftest oracle, commit hygiene)
that keep this area fixable.

The current safety model, for reference — these are the invariants everything below
is judged against:

1. Workers only **read** the world; the main thread is the only world **mutator** and
   must `streamBarrier()` (drain workers) before any mutation.
2. The background **relight** may run while the main thread renders, because it only
   writes the edge light slab, disjoint from what the main thread reads; the window
   must not be mutated while it runs (every recenter/edit path joins it first).
3. The background **pregen** reads only the immutable generator + save files — never
   window state — so it may overlap anything except a window move.
4. Worker mesh results are version-stamped per slot; a stale result is discarded, so
   re-requesting a mesh is always safe.

---

## Findings

### High

**R1 — Light-falloff settings change races every other thread.**
`src/core/App.cpp:2667-2673` (Esc-menu sliders) and `App.cpp:419-425` (`applySettings`):
`world_.setLightFalloff()` recomputes **both light fields** — a full world mutation —
with no `relightFuture_` join and no `streamBarrier()`. If the player drags the
slider while a relight is in flight, two threads write the same light vectors; if
mesh workers have jobs, they read light mid-rewrite. Violates invariants 1 and 2.
It works today only because the menu is usually opened while standing still.
*Fix:* join `relightFuture_`, then `streamBarrier()`, before `setLightFalloff()` —
the same preamble `editBlocks` uses. (At startup `applySettings` is safe only because
`worldConfigWithSettings` pre-applies the values so the call returns false — worth a
comment there pointing at this hazard.)

**R2 — `remeshAll()` after a falloff change is a multi-second main-thread stall.**
`src/render/WorldRenderer.cpp:457-475` iterates the whole window calling
`uploadChunkMesh()`, and each `installMesh` → `Buffer::createDeviceLocal` does its
own GPU submit+wait — exactly the per-chunk round-trip pattern the startup work
eliminated. With a 33×33×16 window that is seconds of frozen frame.
*Fix:* reuse the melt-in machinery: bump versions and `streamRemesh()` the whole
window, let `streamPump` + `recordPendingUploads` spread it over frames (the
`startupMelt_` boost generalizes to "large backlog" if a faster melt is wanted).

### Medium

**R3 — Edits hard-block on an in-flight relight.**
`src/core/App.cpp:614, 658, 677, 832`: `breakBlockAt` / `placeBlockAt` /
`reshapeBlockAt` / `tickLiquids` all start with
`worldRenderer_.streamRemesh(relightFuture_.get())` — `.get()` **blocks the frame**
until the background edge relight finishes (tens of ms). Mining a block in the
seconds after crossing a chunk boundary stutters. The join is required for
correctness (invariant 2) but doesn't have to be synchronous.
*Fix:* if the future isn't ready, queue the edit and apply it next frame (edits are
already discrete events); for `tickLiquids`, simply skip the tick — flow resumes
0.2s later.

**R4 — Every block edit drains the entire GPU.**
`src/render/WorldRenderer.cpp:434-470`: `remeshChunk` / `remeshChunks` call
`vkDeviceWaitIdle` before swapping buffers. The codebase already has the safe
alternative (deferred buffer retirement + frame-integrated upload:
`installMeshBatch` → `recordPendingUploads`, used by streaming and liquids).
On a busy GPU this wait is a visible hitch per mining/placing action.
*Fix:* route edit remeshes through the deferred path too (mesh on the main thread
for latency if desired, but install via `installMeshBatch`). `remeshAll` is covered
by R2.

**R5 — The window step can starve behind worker backlogs.**
`src/core/App.cpp:1510, 1525`: applying a pregen strip (and the teleport path) is
gated on `streamWorkersIdle()`. A sustained remesh source — a large water flood
re-queuing chunks every 0.2s tick — can keep `jobsOutstanding_ > 0` for a long
time, postponing the window step indefinitely while the player keeps moving toward
(and past) the window edge.
*Fix:* time-box the gate: after N postponed frames, `streamBarrier()` anyway (a
bounded, usually-small wait) and apply. Version-stamping (invariant 4) already makes
the workers' in-flight results safely discardable.

**R6 — Seam-margin light reads during background relight are formally racy.**
`src/world/World.cpp` (`shiftColumn` relight-box recording): the relight slab
deliberately reaches ~16 blocks into the *retained* interior so light bleeds across
the seam. While the background flood writes that margin, main-thread light reads
near the seam (e.g. liquid-tick meshing in the `stream_workers: 0` configuration,
or `lightAt` queries) can see torn values. Visually benign (uint8 light levels,
self-corrects on the post-relight remesh) but it is a data race by the letter and
will light up TSan if that's ever run.
*Fix:* document the accepted tear in World.h alongside invariant 2, or keep a
"relight-pending margin" the main-thread paths treat as off-limits.

### Low / debt

**R7 — Streaming budgets are magic numbers, violating the project's own rule.**
`docs/CONFIGURATION.md` says tunables live in documented YAML. But: `streamPump(12)`
(`App.cpp:1562`), melt boost `64` and `kCoreRadius = 5` (`WorldRenderer.cpp`),
upload slice `kSlice = 384` (`WorldRenderer.cpp:389`), liquid `kMaxFills/kScan/kMaxLevel`
(`App.cpp:840-847`). These are exactly the knobs someone will want to tune per
machine. *Fix:* a `streaming:` block in world.yaml.

**R8 — `record()` walks all 17,424 mesh slots twice per frame.**
`src/render/WorldRenderer.cpp:780, 808` (opaque + water passes) iterate every slot
and frustum-test each non-empty one. ~0.1-0.5ms/frame today — fine, but it scales
with window volume, not visible chunks. *Fix when it matters:* maintain a compact
non-empty list updated in `swapChunkBuffers`, and skip the water pass entirely when
no chunk has water (common above ground).

**R9 — App.cpp is a ~2,950-line god object.**
`App::run` alone contains input handling, ~15 `VG_*` debug hooks, the streaming
pipeline orchestration, the frame profiler, and the draw lambdas; the settings menu
and crafting UI live in the same file. The streaming orchestration (pregen/relight
futures + gating — the most invariant-laden code in the project) deserves its own
small class so its rules are stated once, not inline in a 60-line frame-loop block.
Extraction order: StreamingCoordinator, DebugHooks, SettingsMenu.

**R10 — The selftest golden hash is dead weight.**
`src/main.cpp` (`--selftest`): the golden is stale vs the current biomes.yaml *and*
worldgen is cross-process non-deterministic (hash varies run-to-run; within-run
`h1==h2` passes), so the golden check can never pass reliably and everyone has
learned to ignore it — which erodes the whole selftest's authority. *Fix:* either
hunt the non-determinism (it also implies saved worlds may not regenerate
identically — worth its own session) or delete the golden and keep only the
in-process regen check, stating why.

**R11 — ~176 modified files are uncommitted, spanning 3+ work sessions.**
Startup perf, framerate/pregen, Blockbench pipeline, water depth darkening, worldgen
experiments, texture regenerations — all in one working tree. This is the single
largest operational risk in the repo: one careless `git checkout -- .` erases weeks.
*Fix:* commit as a logical series (worldgen data, engine perf, streaming pregen,
Blockbench, assets), then keep sessions committed.

**R12 — The octave clamp guards one axis of config explosions; amplitude is the other.**
`src/world/NoiseStack.h` now clamps invisible octaves (good), but the other half of
the 145s-startup incident was `terrain3d.amplitude: 96`, which put every cell of
every column inside the expensive density band. Nothing warns about that today.
*Fix:* log a load-time warning when `amplitude` exceeds some fraction of world
height (or when the density band covers >50% of the column), naming the cost.

---

## What's solid (keep doing this)

- **Version-stamped worker results** (`meshVersion_`) make "newest request wins"
  trivially correct and let every other system re-request meshes without ceremony.
- **Deferred buffer retirement** (`retired_`, frames-in-flight aging) +
  frame-integrated uploads (`recordPendingUploads`) — the right Vulkan lifetime
  pattern, already proven by streaming/liquids; R2/R4 are just about using it everywhere.
- **Worldgen as a pure function of (seed, coords)** is what made strip pregeneration
  a safe afternoon refactor instead of a rewrite; `generateColumnInto` writing through
  caller pointers keeps it that way. Protect this property in review.
- The gated `VG_*` instrumentation (`VG_MESH_TIME`, `VG_FRAME_TIME`, `VG_AUTOWALK`)
  made both perf investigations measurement-driven; cheap to keep, worth using.

## Suggested fix order

1. **R1 + R2 together** (one change: barrier+join, then streamed remesh-all) — the
   only High-severity race, and the fix removes a giant stall too.
2. **R11** — commit the tree before touching anything else.
3. **R4**, then **R3** — the two per-edit hitch sources, sharing the deferred path.
4. **R5** — bounded gate so the window can't starve.
5. **R7, R10** — convention/oracle debt, both small.
6. **R6, R8, R9, R12** — opportunistic, or when the relevant area is next touched.
