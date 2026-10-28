#pragma once

#include "ui/HudViewState.hpp"

class GameHUD
{
public:
    bool initialize(const char* rmlPath);
    void shutdown();
    void updateView(const FpsHud::HudViewState& view);

private:
    void* m_document = nullptr;
    void* m_model = nullptr;
    bool m_document_visible = false;
    FpsHud::ResourceLifecycle m_resource_lifecycle{};
    FpsHud::HudViewStateCache m_view_cache{};
};
