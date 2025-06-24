cmake_minimum_required(VERSION 3.10)
project(sdl-test)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the executable name
set(TARGET_NAME sdl-test)

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.hpp")
file(GLOB_RECURSE MODELS "models/*.hpp")

# Create the executable
add_executable(${TARGET_NAME} ${SOURCES} ${MODELS})

# Include directories
target_include_directories(${TARGET_NAME} PRIVATE
    "include"
    "include/SDL2"
    "include/SOIL"
    "include/irrlicht"
    "include/stb"  # Add stb_image include directory
    "src"
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Library directories
target_link_directories(${TARGET_NAME} PRIVATE
    "lib"
)

# Find OpenGL
find_package(OpenGL REQUIRED)

# Link libraries
target_link_libraries(${TARGET_NAME}
    # SDL2 - Only link SDL2.lib, NOT SDL2main.lib for custom main
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/x86/SDL2.lib"
    
    # OpenGL
    ${OPENGL_LIBRARIES}
    
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
    set_target_properties(${TARGET_NAME} PROPERTIES
        WIN32_EXECUTABLE FALSE  # Set to FALSE if you want console output
    )
    
    target_compile_definitions(${TARGET_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        SDL_MAIN_HANDLED  # Tell SDL we're handling main ourselves
    )
endif()