#build static
set(BUILD_SHARED_LIBS OFF)

add_subdirectory(SDL2)

#model loaders
add_subdirectory(tinyobjloader)
add_subdirectory(tinygltf-2.9.6)

add_subdirectory(spdlog-1.15.3)


add_subdirectory(entt-3.15.0)
add_subdirectory(miniaudio)
add_subdirectory(glm)
add_subdirectory(freetype-master)


# Make sure the target "freetype" exists in freetype-master CMakeLists
add_library(Freetype::Freetype ALIAS freetype)

# Set backend for RmlUi to legacy OpenGL on Windows
set(RMLUI_BACKEND "Win32_GL2" CACHE STRING "Set RmlUi backend to OpenGL 1.x (legacy)")

add_subdirectory(RmlUi-6.1)