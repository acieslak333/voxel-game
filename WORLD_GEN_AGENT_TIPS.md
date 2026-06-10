# The Coding Agent's Playbook for Infinite Procedural Voxel World Generation in C++

## TL;DR
- **Determinism is the non-negotiable foundation.** Every generation step must be a pure function of `(seed, world coordinate)` — never of chunk visit order, thread timing, or floating-point accumulation. The single most common class of bugs in infinite voxel worlds is non-deterministic or order-dependent generation that produces seams and "different terrain depending on approach direction." Use spatial hashing (e.g., FNV-1a or SplitMix64) to derive per-chunk/per-feature seeds.
- **The modern stack is: OpenSimplex2/FastNoiseLite for noise → density-function (3D) terrain with splines → multi-pass pipeline (shape → surface → caves → features → structures) → binary greedy meshing with per-vertex AO → multithreaded async chunk streaming with a producer/consumer queue.** Prefer OpenSimplex2 over classic Perlin to avoid axis-aligned directional artifacts (Perlin's relevant patent expired January 8, 2022, so patents are no longer a blocker, but the artifacts remain).
- **When reviewing or modifying code, run the checklist in §6.** Watch for: chunk-boundary off-by-one, neighbor-data needed for meshing/AO, integer overflow at large coordinates, float precision far from origin, thread-unsafe shared chunk maps, and any RNG seeded from mutable global state.

---

## Key Findings

1. **Use OpenSimplex2 (via FastNoiseLite) as the default noise primitive.** It avoids the grid-aligned directional artifacts of classic Perlin noise and the (now-expired) simplex patent issues. FastNoiseLite is header-only, portable C++ and supports OpenSimplex2, OpenSimplex2S, Perlin, Value, Value-Cubic, and Cellular noise plus fractal (fBm/ridged/billow) and domain-warp options. For heavy batch workloads, FastNoise2 (node-graph, SIMD via FastSIMD) is several times faster.

2. **Modern terrain is density-function-based, not heightmap-based.** Minecraft 1.18+ uses 3D density functions combined through splines of continentalness, erosion, and peaks-and-valleys (weirdness) noise to produce overhangs, caves, and cliffs that a 2D heightmap cannot. Heightmaps are still the right choice for simpler games or as a fast LOD approximation.

3. **Binary greedy meshing is the current state of the art for blocky meshes**, processing 64 faces at a time with bitwise operations and producing ~8 bytes per quad. Per the cgerikj/binary-greedy-meshing README: *"Execution time for a single chunk typically ranges from 50us to 200us. The chunks in the screenshot were meshed at an average of 74us single-threaded and 108us in a thread pool. (Ryzen 3800x)."* For smooth terrain, surface nets (easy, robust) or dual contouring (sharp features, needs Hermite data) are the choices, with the Transvoxel algorithm for crack-free LOD transitions.

4. **The chunk lifecycle must be an explicit state machine** with async generation/meshing on worker threads and a thread-safe handoff to the main thread. Never integrate generated data on a worker thread without synchronization.

5. **Cross-chunk structures (trees, villages) are the canonical determinism trap.** Two robust solutions exist: (a) deterministic "guessing" of neighbor features without generating neighbors, or (b) explicit multi-pass generation with neighbor access — but the latter requires careful ordering rules to stay reproducible.

---

## Details

### 1. Noise-Based Terrain Generation

**Noise families and when to use each:**
- **Value noise** — cheapest; interpolates random values on a lattice. Blockier/lower quality. Fine for cheap detail layers or masks.
- **Gradient noise (Perlin)** — interpolates random gradients. Smooth, but exhibits axis-aligned directional artifacts, especially noticeable at high frequency. The relevant patent expired January 8, 2022.
- **Simplex / OpenSimplex2 / OpenSimplex2S** — simplex grid reduces directional bias and scales better to higher dimensions. OpenSimplex2 (2019) and OpenSimplex2S (smoother variant) were developed by Kurt Spencer to be visually isotropic and patent-free. **This is the recommended default.**
- **Worley/cellular (Voronoi) noise** — distance-to-feature-points. Use for cracked/scaly textures, biome region cells, or as a cave/ore mask input.

**Fractal Brownian Motion (fBm):** Sum N octaves of noise, each at higher frequency (× lacunarity, typically 2.0) and lower amplitude (× gain/persistence, typically 0.5). More octaves = more fine detail at higher cost; 3–4 octaves for distant/background, 6–8 for hero terrain. **Practical tip from production shader code:** use lacunarity 2.01–2.04 rather than exactly 2.0, and apply a small rotation matrix between octaves, to avoid the lattice points of successive octaves aligning and creating artifacts.

```cpp
float fbm(glm::vec3 p, int octaves, float lacunarity, float gain) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum  += amp * noise3(p * freq);   // noise3 in [-1,1]
        norm += amp;
        freq *= lacunarity;
        amp  *= gain;
    }
    return sum / norm;                     // normalize to [-1,1]
}
```

**Variants:**
- **Ridged noise:** `1 - |noise|` (then often squared) — produces sharp mountain ridges and river networks.
- **Billowy noise:** `|noise|` — produces puffy, cloud/hill shapes.
- **Terraced/terrace function:** quantize the height with `floor(h * steps) / steps`, optionally smoothstep within each band, for stepped/plateau terrain.
- **Domain warping:** distort the input coordinates with noise before sampling: `f(p + k * fbm(p))`. This is the single highest-impact trick for making terrain look organic and non-repetitive (Inigo Quilez's canonical technique). Nest it (warp the warp) for stronger flow.

**Recommended libraries:**
- **FastNoiseLite** — header-only, portable, the default pick. Supports all the above.
- **FastNoise2** — node-graph + SIMD (FastSIMD picks the best instruction set at runtime); use for batch generation throughput. Note: on Windows, ClangCL is recommended over MSVC, which has SIMD compiler bugs that can cause incorrect generation.
- **libnoise** — older, classic modular library; still usable but unmaintained relative to the above.
- **stb_perlin.h** — single-file, trivial to drop in for basic Perlin/fBm when you don't need OpenSimplex.

**Seeding and determinism (critical):** The same `(seed, x, y, z)` must always produce the same value, regardless of when or on what thread it is computed. Pass the seed into the noise object; never seed noise from a global mutable RNG. For per-feature randomness (e.g., "should a tree spawn at this column?"), hash the coordinates into a local seed using a good integer mixer — **SplitMix64** (default Java PRNG, passes BigCrush, very fast) or **PCG**, or **FNV-1a** for a simple spatial hash. Avoid `std::rand()` and avoid seeding `std::mt19937` from a shared, advancing global state in generation code.

```cpp
// Deterministic per-coordinate seed via SplitMix64 mixing
static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
inline uint64_t hashCoord(uint64_t seed, int32_t x, int32_t y, int32_t z) {
    uint64_t h = splitmix64(seed ^ 0x100000001B3ull);
    h = splitmix64(h ^ (uint64_t)(uint32_t)x);
    h = splitmix64(h ^ (uint64_t)(uint32_t)y);
    h = splitmix64(h ^ (uint64_t)(uint32_t)z);
    return h;
}
```
**Salt sub-systems with distinct constants** (terrain vs. caves vs. ores vs. trees each get a different per-layer salt) so that layers are statistically independent yet fully reproducible.

### 2. Chunking and Infinite World Architecture

**Chunk representation.** Common layouts: cubic chunks (16³ or 32³) or tall column-sections (Minecraft uses 16×384×16 columns subdivided into 16×16×16 sub-chunks). Cubic chunks (e.g., 32³) are simpler for fully-3D worlds and LOD. Reference points: Luanti/Minetest uses 16³ MapBlocks; Vintage Story uses 32³; vkguide's Ascendant engine uses 8×8×8; the binary-greedy-meshing reference supports up to 64³.

**Coordinate conversions.** Use floor division and positive modulo — this is a frequent bug site with negative coordinates:
```cpp
constexpr int CS = 32;                         // chunk size
inline int floordiv(int a, int b){ int q=a/b; if((a%b)!=0 && ((a<0)!=(b<0))) --q; return q; }
inline int chunkCoord(int world){ return floordiv(world, CS); }
inline int localCoord(int world){ int m = world % CS; return m < 0 ? m + CS : m; } // never negative
```
A naive `world / CS` and `world % CS` are wrong for negative coordinates (truncation toward zero), producing a one-chunk seam at the origin axes — a classic review catch.

**Chunk lifecycle / state machine.** Model each chunk with explicit states and only advance on the correct thread:
`UNLOADED → QUEUED_GEN → GENERATING (worker) → GENERATED → QUEUED_MESH → MESHING (worker) → MESHED → UPLOADING (GPU) → ACTIVE → QUEUED_UNLOAD → UNLOADED`. Chunks that are not fully generated must not participate in gameplay or be modifiable by neighbors.

**Streaming and threading.** The robust, widely-used pattern: a thread-safe **request queue** (main → workers) and a thread-safe **result queue** (workers → main). Workers generate/mesh; the main thread drains results each frame and integrates them (uploads GPU buffers, inserts into the world map). This separation means the shared world map is only mutated on one thread, avoiding most data races. Use double-buffering for chunk data that can be read by render while being rewritten. Lock-free MPMC queues or work-stealing pools improve scaling; an atomic fetch-add "claim next job" counter is a simple, effective load balancer.

**Memory management.** Pool/recycle chunk allocations (chunk pooling) to avoid churn; use arenas for transient meshing scratch buffers. Store voxels in flat arrays for cache-friendliness (the single most cache-friendly layout). For sparse/empty regions use sparse storage.

**Voxel storage / compression:**
- **Flat array** — `block[x + CS*(y + CS*z)]` or Morton order. Fastest access, most memory. The default for active chunks.
- **Palette compression** — store a per-chunk palette of distinct block types + indices using only `ceil(log2(paletteSize))` bits per voxel. This is what modern Minecraft uses; dramatically reduces memory for chunks with few block types.
- **Run-length encoding (RLE)** — encode runs of identical voxels; excellent for storage/serialization and homogeneous regions.
- **Sparse Voxel Octree (SVO)** — hierarchical, prunes empty space; implicit coordinates. Great for storage and raycasting/LOD, more complex to edit. SVDAGs merge identical subtrees for extreme compression.
- **Bricks** — fixed small blocks (e.g., 8³) pointed to by an octree; good GPU streaming granularity.

**Level of Detail (LOD).** Distant chunks at reduced resolution. For blocky terrain, render distant chunks with larger voxels or a heightmap shell. For smooth terrain, halve resolution per LOD level and use **Transvoxel** transition cells to stitch crack-free seams between LOD levels (each LOD reduces volume resolution by 2×). Octree-based LOD (one octree node per chunk) and clipmaps (concentric rings of resolution around the player) are the standard structures.

### 3. Meshing

**Naive/culled meshing.** Emit a face only between a solid voxel and air/transparent neighbor. This eliminates interior faces (the easy 6× reduction). **Critical gotcha:** to cull faces at chunk borders correctly, the mesher must read the neighbor chunk's edge voxels. Either keep a 1-voxel apron/padding (copy neighbor edges into a padded `CS+2` buffer) or query neighbors directly. Missing this produces visible faces at every chunk boundary (a very common review catch).

**Greedy meshing.** Merge coplanar, identical, same-AO faces into larger quads, drastically reducing vertex count. Per 0fps (Mikola Lysenko), "Meshing in a Minecraft Game (Part 2)": *"Naive: 26536 verts, 6634 quads. Greedy: 7932 verts, 1983 quads."* The algorithm sweeps each of the 3 axis directions, builds a 2D mask of the slice, and greedily grows maximal rectangles. **Binary greedy meshing** (cgerikj) is the modern high-performance variant: build a 64-bit occupancy mask per row, cull 64 faces at once with bit ops, then merge 64 at a time. Reported single-chunk times ~50–200 µs (74 µs single-threaded average on a Ryzen 3800X for the demo scene).

**Faces can only be merged when they share block type AND ambient-occlusion values** — this is why AO must be computed before/within the merge test.

**Smooth voxels:**
- **Marching cubes** — classic; 256 cases via lookup table (15 base classes). Produces many triangles; needs Transvoxel for LOD seams.
- **Surface nets (naive)** — place one vertex per boundary cell, connect neighbors. Easy, fast, smaller meshes, robust on non-distance fields. **Recommended default for smooth terrain.**
- **Dual contouring** — surface nets + Hermite data (gradients) to reproduce sharp features (90° edges). Solve a small QEF per cell. Works naturally across differently-sized octree leaves, so chunk seams/LOD are handled without a separate stitching algorithm — its main advantage over marching cubes.

**Transvoxel.** Eric Lengyel's algorithm (2009) for seamless LOD: standard marching-cubes cells in the interior, plus special **transition cells** (512 cases → 73 equivalence classes) at boundaries between resolution levels. Use the published lookup tables (transvoxel.org); transition cells occupy ~0.5 of the half-resolution cell to keep shading sane.

**Ambient occlusion (per-vertex).** For each vertex, AO is a function of the 3 neighboring voxels (two sides + corner): `ao = side1 && side2 ? 0 : 3 - (side1 + side2 + corner)`. **Anisotropy fix:** because a quad is split into two triangles, AO interpolates wrong on one diagonal. Flip the quad's triangulation when `a00 + a11 > a01 + a10` (orient the split toward the darkest corner). This integrates cleanly with greedy meshing because AO is constant along merged edges.

**Lighting propagation.** Flood-fill lighting (BFS along the 6 faces, decrementing light level) for block light and skylight, as popularized by Minecraft and detailed by Ben Arnold. Two channels (sky + block) allow day/night without remeshing. Spreading order doesn't affect the result as long as it stays within finalized chunks.

**GPU meshing & modern rendering.**
- **Vertex pulling:** drop fixed vertex attributes; the vertex shader reads packed vertex data from an SSBO indexed by `gl_VertexID`. Enables tight bit-packing and one big buffer for all chunks.
- **Multi-draw-indirect (MDI):** `glMultiDrawElementsIndirect` / DAIC structs render all chunks in effectively one draw call. Per Nick McDonald's "High Performance Voxel Engine: Vertex Pooling" (nickmcd.me): *"A vertex pool class is implementable in about 350 lines of code and offers an up to 2x speed increase for static scenes ... it improves frames times by up to 40% and meshing times by up to 25% due to lower driver overhead and better garbage collection for an identical geometry."* Splitting each chunk mesh into **6 orientation buckets** and not issuing the 3 back-facing draws ("the draw call is never even issued") beat both hardware culling and zeroing index counts.
- **GPU-driven culling:** a compute shader tests per-chunk bounds and builds the indirect draw buffer; essential at scale. Per vkguide.dev (Project Ascendant): *"In the Ascendant screenshots shown, there can be up to around 400,000 chunks, so culling on CPU for such a high number is a non-starter."* Use a deferred / visibility-buffer renderer with a simple pixel shader since voxel geometry is dense.
- **Mesh shaders** are the modern successor to vertex pulling for GPU-driven meshlet processing.

**Mesh data layout / vertex packing.** Pack aggressively. The binary-greedy-meshing reference uses **8 bytes per quad**: the first 32-bit word packs 6-bit x + 6-bit y + 6-bit z + 6-bit width + 6-bit height (fits a 64³ chunk), and the second word uses 8 bits for block type. A common single-face 32-bit layout packs ~5–6 bits per axis position + 3 bits face/normal + 2 bits AO + remaining bits for block/texture ID. voxel.wiki shows a 10/10/10-bit position packing into one int (`x | y<<10 | z<<20`). Unpacking bit-packed integers in the vertex shader is cheap and the memory-bandwidth savings on the GPU are large. (Note: binary-greedy-meshing v2 dropped baked AO; AO bits are a v1-style addition you add yourself.)

### 4. Advanced World Generation

**Biome systems.** The modern approach is **multi-noise / Whittaker-style assignment**: sample several independent low-frequency noise fields and map the N-dimensional point to a biome. Per the Minecraft Wiki: *"Biome generation in the Overworld is based on 6 parameters: temperature, humidity (aka. vegetation), continentalness (aka. continents), erosion, weirdness (aka. ridges), and depth."* The game picks the biome whose defined hypercube is nearest the sampled point. Crucially, *"Temperature is a noise parameter used only in biome generation and does not affect terrain generation"* (likewise humidity), while continentalness/erosion/weirdness/depth **also drive terrain shape**, creating a soft link between biome and terrain. To make biomes 4× larger, Minecraft slows the noise sampling by 4× for most parameters. **Extensibility caveat:** naively inserting a new biome into a fixed multi-noise table causes distribution problems because the parameter space is already sliced; mods use "layers/slices" (TerraBlender) or non-linear "squishing" of biome space (BiomeSquisher) to add biomes without distorting others.

**Terrain via splines.** Map each shaping noise through a **spline** (control points) to terrain height/offset and a "squashing factor," then combine. E.g., continentalness spline gives base land/ocean height; erosion spline flattens; peaks-and-valleys adds ridges. The density at a voxel is `offset - y`, biased so density decreases with height; the squashing factor controls how sharply terrain transitions from solid to air (high squash ≈ flat). This is what produces dramatic-yet-traversable terrain.

**Caves.**
- **3D Perlin worms** — trace tunnels along the path of 3D noise / pseudo-random walk.
- **3D noise caves (Minecraft "cheese/spaghetti/noodle"):** sample 3D noise as a density field. Carve where density crosses a threshold. **Cheese caves** = large pockets (the "white" regions of the noise); **spaghetti caves** = the thin boundary band between high/low noise (long winding tunnels); **noodle caves** = thinner spaghetti (1–5 blocks wide). Controlled by frequency, hollowness, and thickness noise maps.
- **Cellular-automata caves** — iterate a birth/survival rule on a random fill; good for organic caverns.
- **Aquifers** — instead of flooding all sub-sea-level air with water, sample separate noise for local water level (cells of 16×40×16) and water-vs-lava (64×40×64), with barrier noise separating bodies. This keeps caves dry/varied below sea level.

**Ores/resources.** Place via thresholded 3D noise or per-position hashing with depth-dependent probability and vein shapes. Minecraft's ore veins use density functions plus a vein-toggle/vein-type noise with vertical range control.

**Structure generation across chunk boundaries (the determinism trap).** A structure (tree, village) may span multiple chunks, but chunks generate independently and in unpredictable order. Two robust patterns:
1. **Deterministic guessing (no neighbor access):** for any chunk, deterministically compute which structures *would* originate in nearby chunks (hash each candidate origin), then render the parts of those structures that overlap the current chunk. Each chunk independently arrives at the same answer. Works well when structure placement depends only on cheap, deterministic inputs (e.g., a 2D heightmap).
2. **Multi-pass with neighbor access (structure references / deferred placement):** generate base terrain for a region first, then a later pass places structures and may write into already-generated neighbors. This is what Minecraft does (it stops game logic in phases near the loaded-map border). **The danger:** if pass-2 on chunk A writes into B, and B writes into A, the outcome can depend on processing order — breaking reproducibility. Mitigate with priority rules (e.g., logs beat leaves) and by only writing into chunks guaranteed not to change later. Store partially-generated chunks in a map; never let gameplay touch them.

**Rivers and erosion.**
- **Rivers** — carve where a ridged/folded weirdness noise crosses zero (Minecraft derives rivers from the folded ridges value), or via flow accumulation on the heightmap.
- **Hydraulic erosion** — simulate water droplets: place droplets at random cells, compute gradient via bilinear interpolation, move downhill, pick up/deposit sediment based on slope/speed/capacity, evaporate over a fixed step count (Sebastian Lague's widely-used implementation, based on Hans Theobald Beyer's method, runs millions of droplets; compute shaders accelerate it). **Caveat for infinite worlds:** classic droplet erosion is a global, iterative process and does not chunk cleanly or stay deterministic per-chunk — usually applied to a bounded heightmap or precomputed region, not streamed infinitely. Note this limitation in review.
- **Terracing** — quantize height as above for plateaus/badlands.

**Heightmap vs. fully-3D density.** Heightmap (2D) generation is simpler, faster, chunk-friendly, and deterministic, but **cannot** produce overhangs, caves, or floating islands. Fully-3D density-function generation can, at higher cost. A common hybrid: 2D for base shape + 3D for caves/overhangs.

**Multi-pass pipeline.** Structure generation as ordered stages with explicit dependencies: **terrain shape (density) → surface rules (grass/dirt/sand by biome) → carvers (caves/ravines) → features (ores, trees, flora) → structures (villages, dungeons)**. Each stage should be deterministic and, ideally, expressible as a pure function of seed + region. Keep stages modular so a new stage can be inserted without touching others.

### 5. Performance and Optimization (C++ specific)

- **SIMD noise.** Use FastNoise2 / FastNoiseSIMD to generate noise in batches (4/8/16 lanes), reportedly up to ~700% faster for simplex in some cases. Generate whole chunk slabs of noise at once rather than per-voxel scalar calls.
- **Data-oriented design.** Prefer **structure-of-arrays (SoA)** for hot loops and GPU upload (better vectorization, fewer cache lines per transaction); array-of-structures (AoS) can still win on CPUs with large caches when you touch all fields of an element together. Keep voxel data in flat contiguous arrays; iterate in memory order (respect your `x + CS*(y + CS*z)` layout — loop the innermost contiguous index in the tightest loop).
- **Multithreading.** Job system / task graph with a thread pool; one job per chunk per stage. Use lock-free queues or work-stealing. Keep generation and meshing off the main thread; only GPU upload + world-map insertion on the main thread.
- **Avoid allocations in hot paths.** Reuse scratch buffers (mesh builders, noise slabs) via per-thread arenas/pools. Pre-size vectors. A large pre-allocated, suballocated GPU "gigabuffer" (as in vkguide's Ascendant) avoids per-chunk buffer creation.
- **Profiling.** Measure meshing µs/chunk, generation µs/chunk, upload time, and frame time separately. Report *relative* speedups (benchmarks are hardware/scene specific). Watch for false sharing on shared atomics and for main-thread stalls draining result queues.

### 6. Code-Review & Refactoring Best Practices — and the Review Checklist

**What to look for in existing voxel/world-gen C++ code:**
- **Determinism bugs:** RNG seeded from a global/advancing state; generation that depends on chunk visit order or thread timing; floating-point accumulation that differs across runs/platforms; using `float` where order-of-operations or compiler fast-math changes results.
- **Chunk-boundary off-by-one:** meshing/AO that doesn't read neighbor edge voxels; `<= CS` vs `< CS`; apron/padding miscopied.
- **Negative-coordinate bugs:** truncating division/modulo for world↔chunk↔local conversions (see §2).
- **Integer overflow at large coordinates:** 32-bit world coords overflow when multiplied for hashing/indexing; promote to 64-bit before multiply; verify hash mixing handles negatives (cast to unsigned consistently).
- **Float precision far from origin:** at ~10⁷ units, `float` precision degrades to ~1 unit, causing vertex jitter/cracks. Render **camera-relative** (subtract camera/chunk origin before converting to float for the GPU), or use 64-bit/fixed-point world coordinates internally. A 48:16 fixed-point gives metric range ~0.029 light-years at ~0.015 mm precision.
- **Thread safety:** shared chunk map mutated from workers; iterating a container while another thread inserts; missing memory ordering on the queue handoff; reading a chunk mesh on the render thread while a worker rewrites it (double-buffer or use state flags).
- **Seams between chunks:** mismatched noise sampling at borders (sample in absolute world space, not chunk-local), LOD transitions without Transvoxel/skirts, lighting that stops at chunk edges.

**Making minimal, safe changes:**
- Treat the generation pipeline as a sequence of pure functions; change one stage at a time.
- Before changing generation, **capture golden/snapshot hashes** of a fixed set of chunks at a fixed seed; re-run after the change. Any unintended hash change is a regression (or an intended, documented worldgen-version bump).
- If a change *must* alter output, gate it behind a **worldgen version** so existing saves regenerate identically.
- Keep noise seeds and salts stable; never reorder how sub-seeds are derived without bumping the version.

**Common pitfalls & how to detect them:**
- "Terrain differs depending on approach direction" → order-dependent generation or neighbor writes (§4 structures).
- Visible chunk-edge walls → missing neighbor data in mesher.
- Jitter/cracks far from spawn → float precision (render camera-relative).
- Cracks at LOD borders → need Transvoxel/skirts/dual-contouring seams.
- AO diamond/asymmetric artifacts → quad not flipped toward darkest corner.
- Periodic, obviously repeating terrain → octave lattice alignment (use 2.01–2.04 lacunarity + rotation) or too-low base frequency.

**Testing strategies:**
- **Unit test noise determinism:** assert `noise(seed, x,y,z)` is bitwise-identical across runs and (ideally) platforms; test known values.
- **Golden/snapshot tests:** hash generated chunk contents for fixed seeds/coords; fail on unexpected change. Luanti ships C++ benchmarks/tests for Map/MapBlock as a model.
- **Property tests:** world↔chunk↔local round-trips for negative and large coordinates; chunk-boundary continuity (a column sampled across a chunk edge is continuous).
- **Visual debugging:** render noise fields/biome maps/density slices to images; use tools like Misode's density-function visualizer as a conceptual model; overlay chunk borders and LOD levels.

**Modularity & extensibility patterns:**
- **Add a new biome:** register it in a data-driven biome table with its parameter ranges; avoid hardcoded `if` chains. Account for parameter-space slicing so existing biomes aren't distorted (layer/slice approach).
- **Add a new noise layer:** give it its own salted seed and expose its parameters (frequency, octaves, amplitude) as data; combine it explicitly in the density-function graph so it can be toggled.
- **Add a new structure type:** implement it as a feature/structure stage object with a deterministic placement predicate and a bounded footprint; register it in the pipeline; handle cross-chunk overlap via §4's patterns.
- **Architecture:** represent generation as a **stage/pass graph** with explicit dependencies (data-driven where possible, like Minecraft's density-function + surface-rule JSON). For engines using ECS, keep voxel-chunk storage as components and generation/meshing as systems/jobs, but keep the pure generation math independent of the ECS so it stays testable and deterministic.

**THE CODE-REVIEW CHECKLIST (run through this on every voxel/worldgen change):**

*Determinism*
- [ ] Is every generation output a pure function of `(seed, world coord)` only?
- [ ] Are all RNGs coordinate-hashed (SplitMix64/PCG), not seeded from global mutable state?
- [ ] Are sub-systems salted with distinct constants?
- [ ] Does any step depend on chunk visit order, thread scheduling, or neighbor-write ordering?
- [ ] If output changed intentionally, is it behind a worldgen-version gate?

*Coordinates & math*
- [ ] World↔chunk↔local conversions use floor-div + positive-mod (correct for negatives)?
- [ ] Coordinates promoted to 64-bit before any multiply used in indexing/hashing?
- [ ] Noise sampled in absolute world space, not chunk-local space?
- [ ] Rendering is camera-relative (or fixed-point world coords) for large worlds?

*Meshing & rendering*
- [ ] Does the mesher read neighbor edge voxels (apron/padding) for boundary faces?
- [ ] Are faces merged only when block type AND AO match?
- [ ] Is the AO diagonal-flip applied (`a00+a11 > a01+a10`)?
- [ ] Are LOD seams handled (Transvoxel / skirts / DC seams)?

*Threading*
- [ ] Is the shared world map mutated on exactly one thread?
- [ ] Is the worker→main handoff via a thread-safe queue with correct memory ordering?
- [ ] Are render-read chunk buffers protected from concurrent worker rewrites?
- [ ] Are partially-generated chunks excluded from gameplay/neighbor edits?

*Performance*
- [ ] No allocations in per-voxel/per-quad hot loops (scratch reused via arenas/pools)?
- [ ] Noise generated in batches/SIMD rather than per-voxel scalar calls?
- [ ] Voxel iteration follows memory layout order?

*Tests*
- [ ] Golden/snapshot hashes exist for fixed seeds and still pass?
- [ ] Determinism unit tests and coordinate round-trip property tests present?

### 7. Reference Material & Prior Art

**Open-source engines to study:**
- **Luanti (formerly Minetest)** — mature C++ voxel engine; 16³ MapBlocks, authoritative client-server, schematic/mapgen system (`mg_schematic.cpp`, `mapgen.h`), Lua modding API. Best real C++ reference for chunk storage, serialization, and mapgen architecture.
- **Terasology** — Java, but instructive architecture for modular worldgen and biomes.
- **Veloren** — Rust, but an excellent modern reference for layered worldgen and erosion.
- **Godot Voxel Tools (Zylann/godot_voxel)** — C++; its docs and the "multipass generation" issue (#545) are an outstanding practical treatment of cross-chunk structures and determinism.
- **cgerikj/binary-greedy-meshing** — C/C++ reference for state-of-the-art binary greedy meshing + vertex pulling + MDI.

**Key blog posts & papers:**
- **0fps (Mikola Lysenko)** — "Meshing in a Minecraft Game" parts 1 & 2 (naive/greedy/monotone), "Smooth Voxel Terrain" (surface nets), "Ambient Occlusion for Minecraft-like Worlds" (AO + anisotropy flip). The canonical meshing references.
- **Nick's Voxel Blog (ngildea)** — dual contouring seams & LOD for chunked terrain.
- **Nick McDonald (nickmcd.me)** — "High Performance Voxel Engine" (vertex pooling, MDI, GPU culling).
- **Eric Lengyel** — Transvoxel algorithm paper + tables (transvoxel.org).
- **Ju, Losasso, Schaefer, Warren** — "Dual Contouring of Hermite Data" (the dual contouring paper).
- **Inigo Quilez** — domain warping and fBm articles (iquilezles.org); The Book of Shaders ch. 13.
- **Henrik Kniberg** — Minecraft world-generation talks/videos (multi-noise biomes, cheese/spaghetti caves) — the authoritative plain-language explanation of modern Minecraft generation.
- **Sebastian Lague** — Coding Adventures: hydraulic erosion, marching cubes (open-source Unity, MIT).
- **Misode** — Minecraft density-function/noise-settings visualizers and docs (conceptual model for data-driven density graphs).
- **Laine & Karras** — "Efficient Sparse Voxel Octrees" (storage/raycasting).

---

## Recommendations

**Staged adoption plan for a coding agent working in an existing C++ voxel codebase:**

1. **First, establish a determinism safety net (do this before any feature work).** Add golden/snapshot tests that hash a fixed set of chunks at a fixed seed, and unit tests asserting noise determinism. Audit all RNG usage for global-state seeding and replace with coordinate-hashed SplitMix64/PCG seeds. *Threshold to proceed:* tests pass and re-running generation twice yields identical hashes.

2. **Fix correctness bugs before optimizing.** In priority order: negative-coordinate world↔chunk↔local conversions; neighbor data in mesher (chunk-edge faces/AO); float precision (switch to camera-relative rendering if vertices jitter far from origin); thread-safety of the chunk map handoff. *Threshold:* no seams, no jitter, no data races under a thread sanitizer.

3. **Modernize noise and terrain shape if needed.** If the code uses classic Perlin and shows directional artifacts, switch to OpenSimplex2 via FastNoiseLite (drop-in). If terrain is heightmap-only and the design needs caves/overhangs, introduce a 3D density-function stage with splines — but add it as a new, version-gated stage, not a rewrite.

4. **Optimize meshing and rendering once correct.** Adopt binary greedy meshing (with AO-aware merging and the diagonal-flip fix). Move to vertex pulling + multi-draw-indirect and (at large view distances) GPU-driven culling. *Threshold:* meshing in the tens-to-hundreds of µs/chunk; single-draw-call chunk rendering.

5. **Parallelize generation/meshing** with a job system and lock-free queues if not already; pre-allocate scratch and GPU buffers. Use FastNoise2/SIMD for batch noise. *Threshold:* no main-thread stalls; worker utilization high; frame time stable while streaming.

6. **Add LOD last.** Heightmap shells or larger voxels for blocky worlds; Transvoxel (or dual-contouring seams) for smooth worlds. *Threshold:* no cracks at LOD boundaries while flying out.

**When to use which technique (decision criteria):**
- Blocky Minecraft-style → flat arrays + palette compression + binary greedy meshing + per-vertex AO.
- Smooth/sculptable terrain → density field + surface nets (or dual contouring for sharp edges) + Transvoxel LOD.
- Need overhangs/caves → 3D density functions; otherwise heightmap is cheaper and simpler.
- Huge view distance / hundreds of thousands of chunks → GPU-driven culling + MDI, consider SVO/SVDAG or ray-marching for the far field.

---

## Caveats

- **Benchmark numbers are hardware- and scene-specific.** The 50–200 µs/chunk meshing and 2×/40%/25% speedup figures come from specific demos (Ryzen 3800X; and, for the vertex-pool figures, a laptop with an integrated GPU, orthographic projection, no frustum culling). Treat relative speedups as portable; treat absolute times as indicative only.
- **Perlin's relevant patent has expired (Jan 8, 2022), so "avoid the patent" is no longer the reason to prefer OpenSimplex2** — the reason is the directional artifacts. Per Wikipedia's Simplex noise article: *"Uses of implementations in 3D and higher for textured image synthesis were covered by U.S. patent 6,867,776 ... which expired on January 8, 2022."*
- **Cross-chunk multi-pass generation is genuinely hard to keep deterministic.** The "guessing" approach is safer but limited to cheaply-predictable placement; the neighbor-write approach needs explicit ordering/priority rules and version gating.
- **Hydraulic erosion does not naturally fit infinite, streamed, deterministic chunking** — it's a global iterative simulation. Use it on bounded regions or precomputed tiles, and document the boundary behavior.
- **binary-greedy-meshing v2 dropped baked AO**; if you copy its 8-byte layout you must add AO yourself (its v1 branch had it). The exact bit fields cited are for a 64³ chunk; adjust bit counts for other chunk sizes.
- **Some sources here are secondary/community** (wikis, blogs). They are excellent engineering references and broadly corroborated, but for exact Minecraft internal constants, treat the Minecraft Wiki values as community-reverse-engineered rather than official specification.
- This guide assumes a CPU-meshed, GPU-rasterized architecture. Pure GPU ray-marched voxel approaches (SVDAG, e.g., the Aokana framework) are an alternative paradigm with different tradeoffs not covered in depth here.