#include "GameHUD.hpp"
#include "Utils/Log.hpp"
#include <cstdio>

static Rml::String FloatStr(float v, int decimals = 1)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

bool GameHUD::initialize(Rml::Context* context, const std::string& rmlPath)
{
    m_context = context;
    if (!m_context)
        return false;

    // Create data model
    Rml::DataModelConstructor constructor = m_context->CreateDataModel("hud");
    if (!constructor)
        return false;

    constructor.Bind("fps", &m_fps);
    constructor.Bind("pos_x", &m_posX);
    constructor.Bind("pos_y", &m_posY);
    constructor.Bind("pos_z", &m_posZ);
    constructor.Bind("speed", &m_speed);
    constructor.Bind("grounded", &m_grounded);
    constructor.Bind("connected", &m_connected);
    constructor.Bind("ping", &m_ping);

    m_modelHandle = constructor.GetModelHandle();

    // Load document
    LOG_ENGINE_INFO("[HUD] Loading document: {}", rmlPath);
    m_document = m_context->LoadDocument(rmlPath);
    if (!m_document)
    {
        LOG_ENGINE_ERROR("[HUD] Failed to load document: {}", rmlPath);
        return false;
    }

    m_document->Show();
    LOG_ENGINE_INFO("[HUD] Document loaded and shown");
    return true;
}

void GameHUD::shutdown()
{
    if (m_document)
    {
        m_document->Close();
        m_document = nullptr;
    }
    if (m_context && m_modelHandle)
    {
        m_context->RemoveDataModel("hud");
        m_modelHandle = {};
    }
    m_context = nullptr;
}

void GameHUD::update(float fps, const glm::vec3& position, float speed, bool grounded,
                     bool connected, float ping)
{
    if (!m_modelHandle)
        return;

    m_fps = (int)fps;
    m_posX = FloatStr(position.x);
    m_posY = FloatStr(position.y);
    m_posZ = FloatStr(position.z);
    m_speed = FloatStr(speed, 2);
    m_grounded = grounded;
    m_connected = connected;
    m_ping = FloatStr(ping, 0);

    m_modelHandle.DirtyAllVariables();
}
