#pragma once

#include "EngineRegistry.hpp"
#include <string>
#include <vector>

// Shows an SDL2+ImGui window listing registered engines for the user to pick.
// Returns the selected engine ID, or empty string if cancelled / no engines.
std::string showEnginePicker(const std::vector<EngineEntry>& engines, const std::string& projectName);
