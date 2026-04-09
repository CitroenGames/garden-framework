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

    // Debug info bindings
    constructor.Bind("fps", &m_fps);
    constructor.Bind("pos_x", &m_posX);
    constructor.Bind("pos_y", &m_posY);
    constructor.Bind("pos_z", &m_posZ);
    constructor.Bind("speed", &m_speed);
    constructor.Bind("grounded", &m_grounded);
    constructor.Bind("connected", &m_connected);
    constructor.Bind("ping", &m_ping);

    // Combat bindings
    constructor.Bind("health", &m_health);
    constructor.Bind("max_health", &m_maxHealth);
    constructor.Bind("ammo_text", &m_ammoText);
    constructor.Bind("alive", &m_alive);
    constructor.Bind("death_timer", &m_deathTimer);
    constructor.Bind("kills", &m_killsText);
    constructor.Bind("deaths", &m_deathsText);
    constructor.Bind("kill_feed", &m_killFeed);
    constructor.Bind("reloading", &m_reloading);

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
                     bool connected, float ping,
                     int32_t health, int32_t max_health, int32_t ammo, int32_t max_ammo,
                     bool alive, float death_timer, int32_t kills, int32_t deaths,
                     const std::string& kill_feed, bool reloading)
{
    if (!m_modelHandle)
        return;

    // Debug info
    m_fps = (int)fps;
    m_posX = FloatStr(position.x);
    m_posY = FloatStr(position.y);
    m_posZ = FloatStr(position.z);
    m_speed = FloatStr(speed, 2);
    m_grounded = grounded;
    m_connected = connected;
    m_ping = FloatStr(ping, 0);

    // Combat info
    m_health = health;
    m_maxHealth = max_health;

    char ammo_buf[32];
    if (reloading) {
        snprintf(ammo_buf, sizeof(ammo_buf), "RELOADING");
    } else {
        snprintf(ammo_buf, sizeof(ammo_buf), "%d / %d", ammo, max_ammo);
    }
    m_ammoText = ammo_buf;

    m_alive = alive;
    m_reloading = reloading;

    if (!alive && death_timer > 0.0f) {
        char timer_buf[32];
        snprintf(timer_buf, sizeof(timer_buf), "Respawning in %.1f...", death_timer);
        m_deathTimer = timer_buf;
    } else {
        m_deathTimer = "";
    }

    char kills_buf[16];
    snprintf(kills_buf, sizeof(kills_buf), "%d", kills);
    m_killsText = kills_buf;

    char deaths_buf[16];
    snprintf(deaths_buf, sizeof(deaths_buf), "%d", deaths);
    m_deathsText = deaths_buf;

    m_killFeed = kill_feed;

    m_modelHandle.DirtyAllVariables();
}
