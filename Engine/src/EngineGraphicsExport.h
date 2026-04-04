#pragma once

#if defined(_WIN32)
#   ifdef ENGINEGRAPHICS_BUILDING_DLL
#       define ENGINE_GRAPHICS_API __declspec(dllexport)
#   else
#       define ENGINE_GRAPHICS_API __declspec(dllimport)
#   endif
#else
#   define ENGINE_GRAPHICS_API __attribute__((visibility("default")))
#endif
