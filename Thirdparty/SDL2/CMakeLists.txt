cmake_minimum_required(VERSION 3.12)
project(SDL2Wrapper)

# Create an interface library for easy consumption
add_library(SDL2Wrapper INTERFACE)

if(WIN32)
    # Windows: Use bundled SDL2 libraries
    # SDL2 include path
    set(SDL2_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/include)

    # SDL2 library paths
    set(SDL2_LIB_DIR ${CMAKE_CURRENT_LIST_DIR}/lib)

    # Auto-detect architecture from CMAKE_SIZEOF_VOID_P
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        # 64-bit
        set(SDL2_LIB_PATH ${SDL2_LIB_DIR}/x64)
        message(STATUS "Using 64-bit SDL2 libraries")
    else()
        # 32-bit
        set(SDL2_LIB_PATH ${SDL2_LIB_DIR}/x86)
        message(STATUS "Using 32-bit SDL2 libraries")
    endif()

    # Fallback: manual override option
    option(FORCE_SDL2_32BIT "Force use of 32-bit SDL2 libraries" OFF)
    if(FORCE_SDL2_32BIT)
        set(SDL2_LIB_PATH ${SDL2_LIB_DIR}/x86)
        message(STATUS "Forcing 32-bit SDL2 libraries")
    endif()

    # Include directories
    target_include_directories(SDL2Wrapper INTERFACE ${SDL2_INCLUDE_DIR})

    # Link libraries
    target_link_libraries(SDL2Wrapper INTERFACE
        ${SDL2_LIB_PATH}/SDL2.lib
        ${SDL2_LIB_PATH}/SDL2main.lib
    )

    # Provide SDL2.dll for runtime
    set(SDL2_DLL ${SDL2_LIB_PATH}/SDL2.dll CACHE FILEPATH "Path to SDL2.dll")

    # Mark useful variables for external use
    set(SDL2_INCLUDE_DIR ${SDL2_INCLUDE_DIR} CACHE PATH "SDL2 Include directory")
    set(SDL2_LIB_PATH ${SDL2_LIB_PATH} CACHE PATH "SDL2 library directory")
    set(SDL2_DLL ${SDL2_DLL} CACHE FILEPATH "Path to SDL2.dll")
else()
    # Linux/Unix: Use system SDL2
    find_package(SDL2 REQUIRED)
    message(STATUS "Using system SDL2: ${SDL2_LIBRARIES}")

    # Include directories
    target_include_directories(SDL2Wrapper INTERFACE ${SDL2_INCLUDE_DIRS})

    # Link libraries
    target_link_libraries(SDL2Wrapper INTERFACE ${SDL2_LIBRARIES})
endif()

# Debug output
if(WIN32)
    message(STATUS "SDL2_LIB_PATH: ${SDL2_LIB_PATH}")
    message(STATUS "Target architecture: ${CMAKE_SIZEOF_VOID_P} bytes pointer")
endif()