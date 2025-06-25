cmake_minimum_required(VERSION 3.10)
project(glad C)

# Set the library name
set(LIB_NAME glad)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Source files
set(SOURCES
    src/glad.c
)

# Create a static library
add_library(${LIB_NAME} STATIC ${SOURCES})

# Specify the public include directory for users of this library
target_include_directories(${LIB_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Set C standard
target_compile_features(${LIB_NAME} PUBLIC c_std_99)
