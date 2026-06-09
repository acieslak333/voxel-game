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

FetchContent_MakeAvailable(glfw glm vulkan_headers)

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
#  The game executable
# -----------------------------------------------------------------------------
add_executable(voxelgame
    src/main.cpp
    src/core/Window.cpp
    src/core/App.cpp
    src/render/VulkanContext.cpp
    src/render/Swapchain.cpp
    src/render/Renderer.cpp
)

target_include_directories(voxelgame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(voxelgame PRIVATE
    glfw
    glm::glm
    Vulkan::Headers
    ${VULKAN_LOADER_LIB}
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
