#build static
set(BUILD_SHARED_LIBS OFF)

# OpenGL loader
add_subdirectory(glad)

add_subdirectory(SDL2)

#model loaders
add_subdirectory(tinyobjloader)
add_subdirectory(tinygltf-2.9.6)

add_subdirectory(spdlog-1.15.3)


add_subdirectory(entt-3.16.0)
add_subdirectory(miniaudio)
add_subdirectory(glm)
add_subdirectory(freetype-master)


# Make sure the target "freetype" exists in freetype-master CMakeLists
add_library(Freetype::Freetype ALIAS freetype)