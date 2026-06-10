# =============================================================================
#  Voxel Survival Game - top-level build script
# =============================================================================
#  Design goals (see AGENT_README.md):
#    * Cross-platform (Windows / Linux / macOS).
#    * Single `cmake` + build command on a clean machine. All third-party
#      libraries are pulled with FetchContent so there is nothing to install
#      by hand except a C++20 compiler and the Vulkan *runtime* loader (which
#      ships with every GPU driver). Even the GLSL->SPIR-V shader compiler is
#      built from source if one is not already on the system.
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(VoxelGame
        VERSION 0.3.0
        DESCRIPTION "First-person voxel survival game (Vulkan + C++20)"
        LANGUAGES CXX)

# -----------------------------------------------------------------------------
#  Language / build configuration
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put all binaries in one predictable place so the executable can find the
# `shaders/` and `assets/` directories relative to itself at runtime.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Default to a Debug build (with Vulkan validation enabled) for single-config
# generators if the user did not pick one explicitly.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

# -----------------------------------------------------------------------------
#  Third-party dependencies (FetchContent)
# -----------------------------------------------------------------------------
include(FetchContent)

# --- GLFW : windowing + input -------------------------------------------------
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
# On Linux, GLFW 3.4 enables the Wayland backend by default, which requires
# `wayland-scanner` at build time. Default to the X11 backend (which only needs
# the common X11 dev headers) so the project configures on minimal systems.
# Re-enable Wayland with -DGLFW_BUILD_WAYLAND=ON if you have the tooling.
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "Build GLFW Wayland backend")
    set(GLFW_BUILD_X11     ON  CACHE BOOL "Build GLFW X11 backend")
endif()

# --- GLM : math (vectors, matrices) ------------------------------------------
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)

# --- Vulkan-Headers : the Vulkan C API headers -------------------------------
#  We always fetch the headers so we are not at the mercy of whatever (if any)
#  Vulkan SDK is installed. The actual loader library is located further down.
FetchContent_Declare(
    vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        vulkan-sdk-1.3.275.0
    GIT_SHALLOW    TRUE
)

# --- yaml-cpp : parse the block-definition data files ------------------------
#  Built from source so the project stays self-contained (no system package
#  required). Block types live in assets/blocks.yaml and are loaded at runtime
#  by BlockRegistry.
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
    GIT_SHALLOW    TRUE
)
set(YAML_CPP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB  OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL        OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw glm vulkan_headers yaml-cpp)

# -----------------------------------------------------------------------------
#  Locate the Vulkan *loader* library (libvulkan / vulkan-1).
#  Headers come from Vulkan-Headers above; here we only need something to link.
#    1. Prefer a full Vulkan SDK if one is installed (gives us the loader and,
#       handily, the glslc/glslangValidator shader compilers).
#    2. Otherwise look for the loader shared library directly.
#    3. As a last resort on Linux, link the versioned `libvulkan.so.1` by path
#       (distros ship the runtime without the unversioned dev symlink).
# -----------------------------------------------------------------------------
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    message(STATUS "Vulkan SDK found: linking the loader from the SDK")
    set(VULKAN_LOADER_LIB Vulkan::Vulkan)
else()
    find_library(VULKAN_LOADER_LIB NAMES vulkan vulkan-1)
    if(NOT VULKAN_LOADER_LIB)
        find_file(VULKAN_LOADER_SO
            NAMES libvulkan.so.1 libvulkan.so
            PATHS /usr/lib /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu
                  /usr/local/lib /usr/lib64)
        if(VULKAN_LOADER_SO)
            set(VULKAN_LOADER_LIB ${VULKAN_LOADER_SO})
        else()
            message(FATAL_ERROR
                "Could not find the Vulkan loader library. Install a GPU driver "
                "with Vulkan support, or the Vulkan SDK from https://vulkan.lunarg.com")
        endif()
    endif()
    message(STATUS "Vulkan loader: ${VULKAN_LOADER_LIB} (headers from Vulkan-Headers)")
endif()

# -----------------------------------------------------------------------------
#  GLSL -> SPIR-V shader compiler
#    1. Prefer a compiler already on the system (glslc or glslangValidator —
#       both ship with the Vulkan SDK).
#    2. Otherwise build glslang from source so the project is fully
#       self-contained (no SDK required just to compile shaders).
# -----------------------------------------------------------------------------
find_program(GLSLC_EXE       NAMES glslc)
find_program(GLSLANG_EXE     NAMES glslangValidator)

set(SHADER_COMPILER_DEP "")   # build dependency, if we build the compiler
if(GLSLC_EXE)
    set(SHADER_COMPILER "${GLSLC_EXE}")
    set(SHADER_COMPILER_STYLE "glslc")
    message(STATUS "Shader compiler: ${GLSLC_EXE} (glslc)")
elseif(GLSLANG_EXE)
    set(SHADER_COMPILER "${GLSLANG_EXE}")
    set(SHADER_COMPILER_STYLE "glslang")
    message(STATUS "Shader compiler: ${GLSLANG_EXE} (glslangValidator)")
else()
    message(STATUS "No system GLSL compiler found; building glslang from source")
    set(GLSLANG_TESTS            OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES  ON  CACHE BOOL "" FORCE) # we need the executable
    set(ENABLE_SPVREMAPPER       OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL              OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT               OFF CACHE BOOL "" FORCE) # avoids needing SPIRV-Tools
    set(BUILD_TESTING            OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG        vulkan-sdk-1.3.275.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(glslang)
    if(TARGET glslang-standalone)
        set(SHADER_COMPILER "$<TARGET_FILE:glslang-standalone>")
        set(SHADER_COMPILER_DEP glslang-standalone)
    elseif(TARGET glslangValidator)
        set(SHADER_COMPILER "$<TARGET_FILE:glslangValidator>")
        set(SHADER_COMPILER_DEP glslangValidator)
    else()
        message(FATAL_ERROR "glslang was built but no standalone compiler target was found")
    endif()
    set(SHADER_COMPILER_STYLE "glslang")
endif()

# Compile a shader from shaders/<name> to <bin>/shaders/<name>.spv at build time.
set(SHADER_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")
set(SPIRV_OUTPUTS "")
function(add_shader SHADER)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER}")
    set(out "${SHADER_OUTPUT_DIR}/${SHADER}.spv")
    if(SHADER_COMPILER_STYLE STREQUAL "glslc")
        set(cmd "${SHADER_COMPILER}" "${src}" -o "${out}")
    else() # glslang/glslangValidator: -V emits Vulkan SPIR-V (stage from extension)
        set(cmd "${SHADER_COMPILER}" -V "${src}" -o "${out}")
    endif()
    add_custom_command(
        OUTPUT  "${out}"
        COMMAND ${cmd}
        DEPENDS "${src}" ${SHADER_COMPILER_DEP}
        COMMENT "Compiling shader ${SHADER}"
        VERBATIM
    )
    set(SPIRV_OUTPUTS ${SPIRV_OUTPUTS} "${out}" PARENT_SCOPE)
endfunction()

add_shader(chunk.vert)
add_shader(chunk.frag)
add_shader(ui.vert)
add_shader(ui.frag)
add_shader(sky.vert)
add_shader(sky.frag)
add_shader(composite.vert)
add_shader(composite.frag)
add_custom_target(shaders DEPENDS ${SPIRV_OUTPUTS})

# -----------------------------------------------------------------------------
#  The game executable
# -----------------------------------------------------------------------------
add_executable(voxelgame
    src/main.cpp
    src/core/Window.cpp
    src/core/App.cpp
    src/core/Input.cpp
    src/core/Palette.cpp
    src/core/DayNight.cpp
    src/core/Settings.cpp
    src/core/Ui.cpp
    src/render/VulkanContext.cpp
    src/render/Swapchain.cpp
    src/render/Renderer.cpp
    src/render/VulkanUtils.cpp
    src/render/Buffer.cpp
    src/render/TextureArray.cpp
    src/render/Pipeline.cpp
    src/render/WorldRenderer.cpp
    src/render/OffscreenTarget.cpp
    src/render/CompositeRenderer.cpp
    src/render/SkyRenderer.cpp
    src/clouds/CloudNoise.cpp
    src/clouds/WeatherMap.cpp
    src/clouds/CloudSystem.cpp
    src/render/UiRenderer.cpp
    src/render/Screenshot.cpp
    src/world/BlockRegistry.cpp
    src/world/ChunkMesher.cpp
    src/world/Noise.cpp
    src/world/NoiseStack.cpp
    src/world/Raycast.cpp
    src/world/TerrainGenerator.cpp
    src/world/World.cpp
    src/world/WorldConfig.cpp
    src/player/Camera.cpp
    src/player/PlayerController.cpp
    src/player/Inventory.cpp
    src/player/Crafting.cpp
    src/player/ChestStore.cpp
)
add_dependencies(voxelgame shaders)

target_include_directories(voxelgame PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

target_link_libraries(voxelgame PRIVATE
    glfw
    glm::glm
    Vulkan::Headers
    ${VULKAN_LOADER_LIB}
    yaml-cpp::yaml-cpp
)

# Copy runtime assets (textures) next to the executable so VG_ASSET_DIR resolves.
add_custom_command(TARGET voxelgame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets"
    COMMENT "Copying assets")

# Tell the program where its compiled shaders and assets live.
target_compile_definitions(voxelgame PRIVATE
    VG_SHADER_DIR="${SHADER_OUTPUT_DIR}"
    VG_ASSET_DIR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets"
)

# Helpful warnings; GLM wants the radians/depth-zero-to-one conventions defined
# globally so every translation unit agrees.
target_compile_definitions(voxelgame PRIVATE
    GLFW_INCLUDE_VULKAN
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE          # Vulkan clip space is [0,1], not [-1,1]
)
if(MSVC)
    target_compile_options(voxelgame PRIVATE /W4 /permissive-)
else()
    target_compile_options(voxelgame PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Enable Vulkan validation layers automatically in Debug builds.
target_compile_definitions(voxelgame PRIVATE
    $<$<CONFIG:Debug>:VG_ENABLE_VALIDATION>
)

message(STATUS "Configured ${PROJECT_NAME} v${PROJECT_VERSION}")
