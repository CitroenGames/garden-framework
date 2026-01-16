# Bug Report: Linker Error - Missing SDL2.lib in Output Directory

## Issue
The build fails with a linker error for the `Game` project in `Release x64` configuration.
`LINK : fatal error LNK1181: cannot open input file 'D:\Github\garden-opengl\x64\Release\SDL2.lib'`

## Environment
- OS: Windows
- Build System: sighmake (generating Visual Studio solution)
- Configuration: Release x64

## Analysis
The project `SDL2` is defined in `Engine/Thirdparty/SDL2/sdl2.buildscript` as a prebuilt library wrapper:
```ini
[project:SDL2]
type = lib
headers = include/**/*.h
public_includes = include
public_libs = lib/x64/SDL2.lib
```

The generated Visual Studio solution attempts to link against `SDL2.lib` assuming it resides in the global build output directory (`x64\Release`), instead of the location specified in `public_libs`.

The file `SDL2.lib` is not copied to `x64\Release` during the build process, causing the linker to fail.

## Expected Behavior
The build tool (`sighmake`) should ensure that:
1. The linker paths are correctly set to point to the source `lib` file.
2. OR the referenced `public_libs` are copied to the output directory so they can be found by the linker.
