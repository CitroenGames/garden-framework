#pragma once

#include <RmlUi/Core.h>
#include <glm/glm.hpp>
#include <string>

class GameHUD
{
public:
    bool initialize(Rml::Context* context, const std::string& rmlPath);
    void shutdown();

    void update(float fps, const glm::vec3& position, float speed, bool grounded,
                bool connected, float ping);

private:
    Rml::Context* m_context = nullptr;
    Rml::ElementDocument* m_document = nullptr;
    Rml::DataModelHandle m_modelHandle;

    // Bound data
    int m_fps = 0;
    Rml::String m_posX = "0.0";
    Rml::String m_posY = "0.0";
    Rml::String m_posZ = "0.0";
    Rml::String m_speed = "0.0";
    bool m_grounded = false;
    bool m_connected = false;
    Rml::String m_ping = "0";
};
