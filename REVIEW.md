# Optimization backlog — voxel-game

The 2026-06-13 code review (findings **R1–R12**: streaming/threading correctness and
the remaining frame-hitch debt) has been **fully resolved** — see git history for the
commits. What remains here is the optimization backlog from that pass: follow-up
performance work, none of it felt slowness today.

Profiled context: startup ~4s (generate dominates), steady state 250–800 fps, worst
frame ~20–30 ms at a chunk-boundary crossing. These are about the remaining generate
cost, RAM, spike tails, and render-distance headroom. Ordered by value.

## Done

- **O1 + O2 — per-column worldgen memo + cliff-probe height cache.** Call-local memos
  in `generateColumnInto` (`columnInfo` / `surfaceY` / `height`, keyed on `(x,z)`)
  dedup the repeated evals across the tree/structure/feature scatter passes and the
  cliff probe. Byte-identical output (selftest hash unchanged, in-process `h1==h2`
  holds); same-environment generate time 1.37s → 0.92s (~33%).
- **O7 — render micro-costs.** Done as part of R8: `record()` iterates a compact
  `drawList_` instead of all ~17k slots twice per frame, and the water pass is skipped
  wholesale when no chunk has water.
- **O3 — palette-index `blockLightColor_` (memory).** The per-cell emitter hue was a
  packed RGBA8 `uint32` (4 B/cell ≈ 285 MB at 33×33×256), almost all "no colored
  light". Emitter colors come from a handful of blocks.yaml entries, so each cell now
  stores a 1-byte palette index into `lightColorPalette_` (built once at construction
  from the registry's emission colors; `emissionColorIndex_[id]` maps a block to its
  slot). ~214 MB saved (4 B → 1 B). Output is byte-identical — palette[idx] is the exact
  packed colour the old code stored, the flood copies indices instead of packed words,
  and reads unpack the same value; `--logictest` + in-process `--selftest` regen pass.
  The palette is built single-threaded so the parallel emitter seed reads it race-free.

- **O4 — boundary frame smoothed (workers below-normal + measured the strip apply).**
  Two parts:
  1. `workerLoop()` drops each mesh-worker thread to `THREAD_PRIORITY_BELOW_NORMAL`
     (Windows; no-op elsewhere — `lowerWorkerThreadPriority` in WorldRenderer.cpp), so a
     window-step's burst of greedy-mesh jobs hands cores back to the main thread instead
     of descheduling it. Pure scheduling hint — workers do identical, version-stamped
     work, no logic change.
  2. The other lever (spread the strip apply over 2-3 frames) was investigated and
     **deliberately not taken**, on measurement, not guesswork. A new `VG_STREAM_TIME`
     stamp (gated, inert otherwise) isolates a window step's synchronous main-thread
     cost: a quiet autowalk crossing many boundaries measured **drain ~0 ms + apply
     0.38-1.43 ms** per step. The heavy work is already off the critical path (relight on
     a background thread; remesh/upload metered by `streamPump`'s `pump_budget`/
     `melt_budget`), and the busy-gate keeps the worker drain at ~0. Spreading a <1.5 ms
     move would also be unsafe: the ring reuses the leaving column's slots for the
     entering column, so the origin advance + slot overwrites must be atomic (a
     half-shifted window would feed render/workers stale data). The noisy ~10 ms `update`
     spikes seen in `VG_FRAME_TIME` are machine contention / other update work, not the
     strip — the CLAUDE.md wall-clock caveat. Verified: `--logictest`/`--selftest` green;
     autowalk steady ~850-1000 fps.

## Remaining (future)

**O5 — GPU buffer pool / VMA instead of one allocation per chunk.**
The per-chunk-buffer design sits near Vulkan's ~4096 allocation ceiling (why upload
batching needs slicing) and churns allocations during streaming. A pooled
suballocator removes the ceiling — the prerequisite for raising view_radius
meaningfully. Structural, medium effort.

**O6 — Interpolated density sampling (approximate; only for view_radius 24–32).**
Sample the terrain3d density stack on a coarse lattice (Minecraft uses 4×8×4) and
trilinearly interpolate: 10–100× on the most expensive function in the game, at the
cost of smoothing sub-4-block density detail. Hold until a bigger window is actually
wanted.

Measured and deliberately NOT worth touching: greedy mesher (0.19 ms/chunk), sky-light
flood (0.38s), per-frame entity baking, the liquid tick.

## What's solid (keep doing this)

- **Version-stamped worker results** (`meshVersion_`) make "newest request wins"
  trivially correct and let every other system re-request meshes without ceremony.
- **Deferred buffer retirement** (`retired_`, frames-in-flight aging) + frame-integrated
  uploads (`recordPendingUploads`) — the right Vulkan lifetime pattern, now used by
  streaming, liquids, AND edit/falloff remeshes (R2/R4).
- **Worldgen as a pure function of (seed, coords)** is what made strip pregeneration a
  safe afternoon refactor instead of a rewrite, and what made the O1/O2 memo a
  byte-identical change; `generateColumnInto` writing through caller pointers keeps it
  that way. Protect this property in review.
- The gated `VG_*` instrumentation (`VG_MESH_TIME`, `VG_FRAME_TIME`, `VG_AUTOWALK`)
  made every perf investigation measurement-driven; cheap to keep, worth using.
- **Streaming orchestration lives in one place** (`App::streamWindow`, R9) with the
  four-actor threading invariants stated in its header — keep new streaming logic
  there, not back inline in the frame loop.
