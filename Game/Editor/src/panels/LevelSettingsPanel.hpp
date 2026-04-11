#pragma once

#include "LevelManager.hpp"

class LevelSettingsPanel
{
public:
    LevelMetadata* metadata = nullptr;

    void draw(bool* p_open = nullptr);
};
