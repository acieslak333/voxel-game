# Voxel Survival Game

A first-person voxel survival game built from scratch in modern **C++20** with
**Vulkan**. The long-term vision is a survival game on a single procedurally
generated island; this repository builds that foundation up in small, verifiable
milestones.

> **Status:** Milestone 0 complete — cross-platform CMake project that opens a
> window, initialises Vulkan, and clears the screen to a solid colour.

---

## Tech stack

| Concern            | Choice                                              |
|--------------------|-----------------------------------------------------|
| Language           | C++20                                               |
| Graphics API       | Vulkan                                              |
| Windowing / input  | GLFW                                                |
| Math               | GLM                                                 |
| Build system       | CMake (cross-platform: Windows, Linux, macOS)       |
| Dependencies       | Pulled automatically with CMake `FetchContent`      |

### Dependencies

These are downloaded and built automatically by CMake the first time you
configure — you do **not** need to install them by hand:

- [GLFW](https://github.com/glfw/glfw) 3.4 — windowing & input
- [GLM](https://github.com/g-truc/glm) 1.0.1 — math
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (sdk-1.3.275) — Vulkan API headers

The only things you must provide yourself:

1. **A C++20 compiler** (GCC 11+, Clang 14+, or MSVC 2022).
2. **CMake 3.20+** and a generator (Ninja or Make on Linux/macOS, Visual Studio on Windows).
3. **The Vulkan runtime loader** — ships with every modern GPU driver. Installing
   the [Vulkan SDK](https://vulkan.lunarg.com/) is the easiest way to get it
   (and it also provides the validation layers used in Debug builds).

---

## Building

The project configures and builds with a single pair of commands on every
platform.

### Linux

Install a compiler, CMake, and the X11 development headers GLFW needs, then build:

```bash
# Debian/Ubuntu — system prerequisites
sudo apt install build-essential cmake ninja-build \
                 xorg-dev libxkbcommon-dev          # X11 headers for GLFW
# Vulkan loader + (optionally) validation layers:
sudo apt install libvulkan1 vulkan-validationlayers

# Configure & build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# Run
./build/bin/voxelgame
```

> GLFW is built against X11 by default here. To use the Wayland backend instead,
> install `wayland-protocols` + `libwayland-dev` and configure with
> `-DGLFW_BUILD_WAYLAND=ON`.

### Windows

Install [Visual Studio 2022](https://visualstudio.microsoft.com/) (with the
"Desktop development with C++" workload) and the
[Vulkan SDK](https://vulkan.lunarg.com/). Then:

```powershell
cmake -S . -B build
cmake --build build --config Debug

.\build\bin\Debug\voxelgame.exe
```

### macOS

Install the [Vulkan SDK](https://vulkan.lunarg.com/) (which includes MoltenVK,
the Vulkan-on-Metal translation layer) and a compiler/CMake (e.g. via Xcode
command-line tools + Homebrew):

```bash
brew install cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

./build/bin/voxelgame
```

---

## Running

Launch the executable from anywhere; it locates its `shaders/` and `assets/`
folders relative to its own location.

| Flag          | Meaning                                                        |
|---------------|---------------------------------------------------------------|
| `--frames N`  | Render `N` frames then exit (used for headless smoke-testing). |

In **Debug** builds the Vulkan validation layers are enabled automatically and
report warnings/errors to the console.

### Headless / CI testing

The project runs without a physical GPU using a virtual display and Mesa's
software Vulkan driver (lavapipe) — handy for CI:

```bash
sudo apt install xvfb mesa-vulkan-drivers
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
xvfb-run -a -s "-screen 0 1280x720x24" ./build/bin/voxelgame --frames 10
```

---

## Project layout

```
src/
  core/      app, window, (input & timing later)
  render/    Vulkan: context (instance/device), swapchain, renderer
  world/     blocks, chunks, meshing, world gen   (later milestones)
  player/    camera, controller                   (later milestones)
shaders/     GLSL compiled to SPIR-V at build time (later milestones)
assets/      textures                             (later milestones)
```

Vulkan setup is split into focused, RAII-wrapped classes
(`VulkanContext`, `Swapchain`, `Renderer`) rather than one giant file, so each
stage is readable on its own.

---

## Roadmap

- [x] **Milestone 0** — Project skeleton: window + Vulkan + clear screen.
- [ ] **Milestone 1** — Render one greedy-meshed, textured chunk.
- [ ] **Milestone 2** — First-person camera, walking + collision, free-fly.
- [ ] **Milestone 3** — Procedural multi-chunk noise terrain.

See [`FUTURE.md`](FUTURE.md) for documented extension points for deferred
features (inventory, crafting, mobs, custom block models, island shaping, …).
