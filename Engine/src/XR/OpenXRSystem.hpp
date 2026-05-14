#pragma once

#include "EngineGraphicsExport.h"

#include <openxr/openxr.h>
#include <cstdint>
#include <string>
#include <vector>

class IRenderAPI;

namespace XR
{
    struct OpenXRInitDesc
    {
        const char* application_name = "Garden";
        uint32_t application_version = 1;
        const char* engine_name = "Garden Framework";
        uint32_t engine_version = 1;
    };

    struct OpenXRViewInfo
    {
        uint32_t recommended_width = 0;
        uint32_t recommended_height = 0;
        uint32_t max_width = 0;
        uint32_t max_height = 0;
        uint32_t recommended_sample_count = 1;
        uint32_t max_sample_count = 1;
    };

    struct OpenXRStatus
    {
        bool initialized = false;
        bool runtime_available = false;
        bool hmd_available = false;
        bool pie_active = false;
        bool backend_supported = false;
        bool session_running = false;
        bool instance_loss_pending = false;
        std::string runtime_name;
        std::string system_name;
        std::string backend_name;
        std::string last_error;
        XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
        uint32_t runtime_version_major = 0;
        uint32_t runtime_version_minor = 0;
        uint32_t runtime_version_patch = 0;
        uint32_t view_count = 0;
        XrEnvironmentBlendMode environment_blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    };

    class ENGINE_GRAPHICS_API OpenXRSystem
    {
    public:
        static OpenXRSystem& get();

        bool initialize(const OpenXRInitDesc& desc = {});
        void shutdown();

        bool beginPIE(IRenderAPI* render_api, bool force_enable = false);
        void endPIE();
        void pollEvents();

        bool isInitialized() const { return m_status.initialized; }
        bool isPIEActive() const { return m_status.pie_active; }
        bool isBackendSupported() const { return m_status.backend_supported; }
        const OpenXRStatus& getStatus() const { return m_status; }
        const std::vector<OpenXRViewInfo>& getViewConfiguration() const { return m_views; }
        const std::vector<std::string>& getEnabledExtensions() const { return m_enabled_extensions; }

        bool isExtensionSupported(const char* extension_name) const;
        std::string resultToString(XrResult result) const;

    private:
        OpenXRSystem() = default;
        ~OpenXRSystem();

        OpenXRSystem(const OpenXRSystem&) = delete;
        OpenXRSystem& operator=(const OpenXRSystem&) = delete;

        bool createInstance(const OpenXRInitDesc& desc);
        bool queryRuntimeAndSystem();
        void queryViewConfiguration();
        void queryEnvironmentBlendMode();
        void setLastError(const std::string& message);
        void resetRuntimeStatus();

        XrInstance m_instance = XR_NULL_HANDLE;
        XrSystemId m_system_id = XR_NULL_SYSTEM_ID;

        std::vector<XrExtensionProperties> m_supported_extensions;
        std::vector<std::string> m_enabled_extensions;
        std::vector<OpenXRViewInfo> m_views;
        OpenXRStatus m_status;
    };
}
