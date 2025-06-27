cmake_minimum_required(VERSION 3.10)
project(sdl-test)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(Thirdparty)

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.hpp")

# Create the executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    "Thirdparty/include"
    "src"
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Find OpenGL
find_package(OpenGL REQUIRED)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    SDL2Wrapper
    ${OPENGL_LIBRARIES}
    tinygltf
    tinyobjloader
    EnTT
    AudioEngine
    glm
    Freetype::Freetype
    rmlui

    # Windows system libraries
    kernel32.lib
    user32.lib
    gdi32.lib
    winspool.lib
    comdlg32.lib
    advapi32.lib
    shell32.lib
    ole32.lib
    oleaut32.lib
    uuid.lib
    odbc32.lib
    odbccp32.lib
)

# Windows-specific settings
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE TRUE  # Set to TRUE for GUI application
    )
    
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        SDL_MAIN_HANDLED 
        NOMINMAX
    )
endif()