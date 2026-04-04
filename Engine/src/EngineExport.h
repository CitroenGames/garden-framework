#pragma once

#if defined(_WIN32)
#   ifdef ENGINECORE_BUILDING_DLL
#       define ENGINE_API __declspec(dllexport)
#   else
#       define ENGINE_API __declspec(dllimport)
#   endif
#else
#   define ENGINE_API __attribute__((visibility("default")))
#endif
