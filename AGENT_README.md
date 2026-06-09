Coding Agent Prompt — C++ Voxel Survival Game

Paste everything below into your coding agent. It is written to be built incrementally: do not skip ahead to later milestones until the current one runs.

Project Overview

Build the foundation of a first-person voxel survival game in modern C++. The long-term vision is a survival game set on a single procedurally generated island, but we are starting from the absolute beginning and building up in verifiable milestones.

Do not build the whole game at once. Complete each milestone, make sure it compiles and runs, then move to the next. After each milestone, stop and summarize what was built and how to run it.

Tech Stack (non-negotiable)





Language: Modern C++ (C++20). Assume an intermediate developer who is rusty — favor clear, well-commented code over clever code.



Graphics API: Vulkan. Yes, this is verbose. Structure the Vulkan setup into clean, separate files (instance, device, swapchain, pipeline, command buffers, sync) so it is readable and maintainable.



Build system: CMake, cross-platform (Windows, Linux, macOS) from the start.



Windowing/input: GLFW.



Math: GLM.



Dependency management: Use CMake FetchContent (or a clearly documented vendored approach) so the project builds with a single cmake + build command on a clean machine. Document all dependencies in the README.

Architecture Principles





Build minimal, design for extensibility. Several systems are deferred (inventory, crafting, mobs, custom block models). Do not implement them yet, but leave clean extension points and document where future code should hook in (use // TODO(future): comments and a FUTURE.md file).



Clear module separation. Suggested top-level structure: 

/src  /core        (app, window, input, timing)  /render      (Vulkan: device, swapchain, pipeline, mesh upload)  /world       (block, chunk, chunk mesher, world gen)  /player      (camera, controller)/assets  /textures    (solid-color PNGs, one per block face for now)/shaders       (GLSL compiled to SPIR-V at build time)




Data-oriented where it matters (chunk storage, meshing) but readable everywhere else.

Core Data Model

Block

Represent a block as a small struct, not just a raw ID, so metadata can grow later:

struct Block {
    uint16_t id;        // block type (air = 0)
    uint8_t  metadata;  // reserved for future use (orientation, state, etc.)
    // Keep this struct small; document that more fields may be added.
};


Maintain a block registry mapping id -> properties (name, is_solid, is_transparent, texture indices per face). This registry is the single source of truth and must be easy to extend with new block types.

Chunk





Fixed-size chunk of 16 x 16 x 16 blocks (configurable via a compile-time constant).



Store blocks in a flat contiguous array with a clear index helper index(x, y, z).



A chunk knows how to: get/set a block, and (re)generate its mesh.

Rendering: Greedy Meshing + Texture Array





Convert a chunk into a mesh using greedy meshing (merge coplanar adjacent faces of the same block type into larger quads). Implement it cleanly and comment the algorithm well, since it is the trickiest part.



Critical texturing note: Because greedy meshing produces quads spanning multiple blocks, a naive texture atlas will stretch the texture across the merged quad. Use a Vulkan texture array (VK_IMAGE_VIEW_TYPE_2D_ARRAY) with UV coordinates that repeat per block unit (texture wrap = repeat, UVs scaled to quad size in block units). Each block face references a layer index in the array.



Textures for now are solid-color PNGs — one simple PNG per block type/face. Load them into the texture array at startup. Architect the loader so that later, some blocks can use custom models (e.g. a furnace) instead of cube faces — note this extension point but do not implement custom models yet.



Only generate faces that are exposed (a face is skipped if the neighboring block is solid/opaque). Greedy meshing should incorporate this culling.

Player & Camera





First-person camera with mouse look.



Walking mode: WASD movement, basic gravity, and simple AABB collision against solid blocks so the player stands on / walks over terrain. Jump with space.



Free-fly mode: toggle with a key (e.g. F) — no gravity, no collision, fly freely for debugging and exploration.



Keep the player controller modular so survival mechanics can attach later.

Survival (minimal)





Implement health only as a single value on the player, with a clean place to add more stats later.



Do not implement hunger, thirst, damage sources, crafting, inventory, or mobs yet. Leave documented hooks for them.

Milestones (build in this order)

Milestone 0 — Project skeleton CMake project that builds cross-platform, opens a GLFW window, initializes Vulkan, and clears the screen to a solid color. Document build steps in the README. Verify it runs before proceeding.

Milestone 1 — Render one chunk Hardcode a single 16x16x16 chunk filled with a couple of block types (e.g. a flat layer of "grass" over "dirt", rest air). Implement the Block struct, chunk storage, the block registry, greedy meshing, the texture array with solid-color PNGs, and the shaders. Render it. This is the key proof-of-concept: a textured, greedy-meshed chunk on screen.

Milestone 2 — Camera & controls Add the first-person camera, walking with gravity + AABB collision against the chunk, and the free-fly toggle.

Milestone 3 — Procedural world generation Replace the hardcoded chunk with terrain from multiple layers of noise (e.g. one noise function for base terrain height, another for biome/material variation). Generate and render an NxNxN grid of chunks around the player. Use a seedable noise library (or a documented Perlin/Simplex implementation). No island shaping yet — just confirm multi-chunk noise terrain works.

Later milestones (do NOT build now, just keep architecture friendly to them): island shaping (land surrounded by water), chunk streaming as the player moves, inventory, crafting, mobs, custom block models, and additional survival stats.

Deliverables for each milestone





Working, compiling code committed in logical files.



Updated README.md with build + run instructions for all three platforms.



A FUTURE.md listing the documented extension points for deferred features.



A short summary of what was built and how to test it.

Coding standards





C++20, RAII for all Vulkan/GLFW resources (wrap handles, no leaks).



Comment non-obvious logic — especially the Vulkan setup and the greedy meshing algorithm — so a rusty intermediate dev can follow it.



Prefer small, single-responsibility files. Avoid one giant main.cpp.

Start with Milestone 0. Stop and report after each milestone.

Make sure to commit and push as acieslak333 not claude-code.
