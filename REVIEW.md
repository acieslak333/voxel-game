# Optimization backlog — voxel-game

The 2026-06-13 code review (findings **R1–R12**: streaming/threading correctness and
the remaining frame-hitch debt) and its follow-up optimization backlog (**O1–O7**) are
both **fully resolved** — see git history for the commits and the per-item write-ups
below. None of it was felt slowness at the time; the work bought RAM, killed the
allocation ceiling, and opened headroom for a larger render distance.

Profiled context (at the time): startup ~4s (generate dominates), steady state 250–800
fps, worst frame ~20–30 ms at a chunk-boundary crossing. The items below targeted the
generate cost, RAM, spike tails, and render-distance headroom.

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

- **O5 — GPU block sub-allocator (removes the per-chunk allocation ceiling).** Every
  Buffer used to do its own `vkAllocateMemory`, so each chunk mesh + each staging
  buffer counted against Vulkan's `maxMemoryAllocationCount` (guaranteed >= 4096, often
  exactly that) — a hard cap on resident chunks and a per-upload alloc/free churn. New
  `GpuAllocator` (src/render/GpuAllocator.{h,cpp}, owned by VulkanContext) hands out
  sub-allocations from a few large 64 MiB `VkDeviceMemory` blocks: keep one (uncapped)
  VkBuffer per chunk but `vkBindBufferMemory` it into a shared block at an offset.
  Freed ranges return to a per-block coalescing free-list and are reused; host-visible
  blocks are persistently mapped (Buffer::map() just returns the sub-allocation ptr,
  unmap is a no-op). Buffer's public API is unchanged — only its internals. No block
  reclaim in this first pass (the streaming working set is bounded, so block count
  reaches a steady high-water well under the ceiling). Measured: 580 chunk meshes + all
  staging now ride **2 blocks** (128 MiB allocated / 46.5 MiB live) vs ~580+ separate
  device allocations — allocation count now scales with bytes, not chunk count.
  Verified under Vulkan validation layers (Debug autowalk): no binding / alignment /
  aliasing / use-after-free errors; Release streaming steady ~1100-1280 fps;
  `--logictest`/`--selftest` green. This is the prerequisite for raising view_radius.
  A `VG_MESH_TIME` line now prints pool block count + allocated/live MiB.

- **O6 — interpolated density sampling (opt-in).** The 3D density stack (the most
  expensive eval in worldgen — run per cell across the ±amplitude band) can now be
  approximated: `TerrainGenerator::mainSolid` samples it only at the corners of a
  world-aligned lattice (cell size `terrain3d.lattice`, default 4×8×4) and trilinearly
  interpolates. Corners are memoised in a fixed-size per-worker thread_local cache —
  pure memoisation (output independent of cache state, so determinism + worker-order
  independence hold), keyed by an exact (epoch, x, y, z) discriminator so a hash
  collision is a clean miss not a wrong value. World-aligned so neighbouring chunks
  share corners (seam-safe; 4/8/4 divide 16 → chunk boundaries land on lattice points).
  Measured (heavy 8-layer biomes.yaml stack): generate **2400 ms → 585 ms (~4.1x)** —
  the density function itself drops ~10-100×; total generate is 4× because the other
  noise (heightmap/climate/features) is now the floor. **Opt-in** via
  `terrain3d.interpolate` (default false): off = byte-identical terrain + stable golden
  + the thread_local cache is never even allocated; on = the determinism check (h1==h2)
  still passes, the golden intentionally changes (it's an approximation). This is the
  headroom O5 unblocked for a larger view_radius. The cost is smoothing density detail
  finer than one cell; horizontal detail is preserved more than vertical (cell is
  taller in Y).

Measured and deliberately NOT worth touching: greedy mesher (0.19 ms/chunk), sky-light
flood (0.38s), per-frame entity baking, the liquid tick.

## Remaining (future)

The O1–O7 backlog from the 2026-06-13 review is fully resolved. No open perf items;
the list above is the record of what was done and what was measured-and-skipped.

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
