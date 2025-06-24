cmake_minimum_required(VERSION 3.10)
project(sdl-test)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the executable name
set(TARGET_NAME sdl-test)

# Source files
file(GLOB_RECURSE SOURCES "*.cpp")

# Create the executable
add_executable(${TARGET_NAME} ${SOURCES})

# Include directories
target_include_directories(${TARGET_NAME} PRIVATE
    "include"
    "include/SDL2"
    "include/SOIL"
    "include/irrlicht"
)

# Library directories
target_link_directories(${TARGET_NAME} PRIVATE
    "lib"
)

# Find OpenGL
find_package(OpenGL REQUIRED)

# For Visual Studio, you would need .lib files instead of .a files
# Replace your MinGW libraries with MSVC equivalents:
target_link_libraries(${TARGET_NAME}
    # MSVC-compiled libraries (you need to obtain these)
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/x86/SDL2main.lib"   
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/x86/SDL2.lib"       
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/x86/SOIL.lib"       
    
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
        WIN32_EXECUTABLE TRUE
    )
    
    target_compile_definitions(${TARGET_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
endif()