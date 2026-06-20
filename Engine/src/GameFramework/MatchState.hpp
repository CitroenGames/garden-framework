#pragma once

#include <string>

namespace GameFramework
{
namespace MatchState
{
    inline constexpr const char* EnteringMap = "EnteringMap";
    inline constexpr const char* WaitingToStart = "WaitingToStart";
    inline constexpr const char* InProgress = "InProgress";
    inline constexpr const char* WaitingPostMatch = "WaitingPostMatch";
    inline constexpr const char* LeavingMap = "LeavingMap";
    inline constexpr const char* Aborted = "Aborted";

    inline int order(const std::string& state)
    {
        if (state == EnteringMap) return 0;
        if (state == WaitingToStart) return 1;
        if (state == InProgress) return 2;
        if (state == WaitingPostMatch) return 3;
        if (state == LeavingMap) return 4;
        if (state == Aborted) return 5;
        return 2;
    }
}
}
