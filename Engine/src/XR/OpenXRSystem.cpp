#include "XR/OpenXRSystem.hpp"

#include "Console/ConVar.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Utils/Log.hpp"

#include <algorithm>
#include <array>
#include <cstring>

CONVAR(xr_enable, 0, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable OpenXR runtime integration");
CONVAR(xr_pie, 1, ConVarFlags::ARCHIVE | ConVarFlags::CLIENT_ONLY,
       "Enable OpenXR runtime probing and event polling while Play In Editor is active");

namespace
{
    constexpr const char* XR_EXT_D3D12 = "XR_KHR_D3D12_enable";
    constexpr const char* XR_EXT_VULKAN2 = "XR_KHR_vulkan_enable2";
    constexpr const char* XR_EXT_VULKAN1 = "XR_KHR_vulkan_enable";
    constexpr const char* XR_EXT_METAL = "XR_KHR_metal_enable";

    void copyOpenXRString(char* dst, size_t dst_size, const char* src)
    {
        if (!dst || dst_size == 0)
            return;

        const char* safe_src = src ? src : "";
        std::strncpy(dst, safe_src, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }

    bool cvarEnabled(const char* name, bool fallback)
    {
        ConVarBase* cvar = ConVarRegistry::get().find(name);
        return cvar ? cvar->getBool() : fallback;
    }

    const char* sessionStateName(XrSessionState state)
    {
        switch (state)
        {
        case XR_SESSION_STATE_UNKNOWN: return "Unknown";
        case XR_SESSION_STATE_IDLE: return "Idle";
        case XR_SESSION_STATE_READY: return "Ready";
        case XR_SESSION_STATE_SYNCHRONIZED: return "Synchronized";
        case XR_SESSION_STATE_VISIBLE: return "Visible";
        case XR_SESSION_STATE_FOCUSED: return "Focused";
        case XR_SESSION_STATE_STOPPING: return "Stopping";
        case XR_SESSION_STATE_LOSS_PENDING: return "Loss Pending";
        case XR_SESSION_STATE_EXITING: return "Exiting";
        default: return "Other";
        }
    }

    const char* blendModeName(XrEnvironmentBlendMode mode)
    {
        switch (mode)
        {
        case XR_ENVIRONMENT_BLEND_MODE_OPAQUE: return "Opaque";
        case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE: return "Additive";
        case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND: return "Alpha Blend";
        default: return "Unknown";
        }
    }
}

namespace XR
{
    OpenXRSystem& OpenXRSystem::get()
    {
        static OpenXRSystem instance;
        return instance;
    }

    OpenXRSystem::~OpenXRSystem()
    {
        shutdown();
    }

    void OpenXRSystem::resetRuntimeStatus()
    {
        m_status = {};
        m_views.clear();
        m_enabled_extensions.clear();
        m_supported_extensions.clear();
        m_system_id = XR_NULL_SYSTEM_ID;
    }

    bool OpenXRSystem::initialize(const OpenXRInitDesc& desc)
    {
        if (m_status.initialized)
            return true;

        resetRuntimeStatus();

        if (!createInstance(desc))
            return false;

        m_status.initialized = true;
        m_status.runtime_available = true;

        if (!queryRuntimeAndSystem())
        {
            LOG_ENGINE_WARN("[OpenXR] Runtime initialized, but no HMD system is currently available: {}",
                            m_status.last_error);
            return true;
        }

        queryViewConfiguration();
        queryEnvironmentBlendMode();

        LOG_ENGINE_INFO("[OpenXR] Runtime '{}' initialized for '{}' ({} stereo views, {}x{} recommended)",
                        m_status.runtime_name,
                        m_status.system_name,
                        m_status.view_count,
                        m_views.empty() ? 0 : m_views[0].recommended_width,
                        m_views.empty() ? 0 : m_views[0].recommended_height);
        return true;
    }

    void OpenXRSystem::shutdown()
    {
        if (m_instance != XR_NULL_HANDLE)
        {
            xrDestroyInstance(m_instance);
            m_instance = XR_NULL_HANDLE;
        }

        resetRuntimeStatus();
    }

    bool OpenXRSystem::createInstance(const OpenXRInitDesc& desc)
    {
        uint32_t extension_count = 0;
        XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr);
        if (XR_FAILED(result))
        {
            setLastError("xrEnumerateInstanceExtensionProperties failed: " + resultToString(result));
            return false;
        }

        m_supported_extensions.assign(extension_count, { XR_TYPE_EXTENSION_PROPERTIES });
        if (extension_count > 0)
        {
            result = xrEnumerateInstanceExtensionProperties(
                nullptr, extension_count, &extension_count, m_supported_extensions.data());
            if (XR_FAILED(result))
            {
                setLastError("xrEnumerateInstanceExtensionProperties failed: " + resultToString(result));
                return false;
            }
        }

        std::vector<const char*> enabled_extensions;
        auto enableIfSupported = [&](const char* extension)
        {
            if (isExtensionSupported(extension))
            {
                enabled_extensions.push_back(extension);
                m_enabled_extensions.emplace_back(extension);
            }
        };

#if defined(_WIN32)
        enableIfSupported(XR_EXT_D3D12);
#endif
        enableIfSupported(XR_EXT_VULKAN2);
        if (!isExtensionSupported(XR_EXT_VULKAN2))
            enableIfSupported(XR_EXT_VULKAN1);
#if defined(__APPLE__)
        enableIfSupported(XR_EXT_METAL);
#endif

        XrApplicationInfo app_info{};
        copyOpenXRString(app_info.applicationName, sizeof(app_info.applicationName), desc.application_name);
        app_info.applicationVersion = desc.application_version;
        copyOpenXRString(app_info.engineName, sizeof(app_info.engineName), desc.engine_name);
        app_info.engineVersion = desc.engine_version;
        app_info.apiVersion = XR_CURRENT_API_VERSION;

        XrInstanceCreateInfo create_info{ XR_TYPE_INSTANCE_CREATE_INFO };
        create_info.applicationInfo = app_info;
        create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
        create_info.enabledExtensionNames = enabled_extensions.empty() ? nullptr : enabled_extensions.data();

        result = xrCreateInstance(&create_info, &m_instance);
        if (XR_FAILED(result))
        {
            m_instance = XR_NULL_HANDLE;
            setLastError("xrCreateInstance failed: " + resultToString(result));
            return false;
        }

        return true;
    }

    bool OpenXRSystem::queryRuntimeAndSystem()
    {
        XrInstanceProperties instance_props{ XR_TYPE_INSTANCE_PROPERTIES };
        XrResult result = xrGetInstanceProperties(m_instance, &instance_props);
        if (XR_FAILED(result))
        {
            setLastError("xrGetInstanceProperties failed: " + resultToString(result));
            return false;
        }

        m_status.runtime_name = instance_props.runtimeName;
        m_status.runtime_version_major = XR_VERSION_MAJOR(instance_props.runtimeVersion);
        m_status.runtime_version_minor = XR_VERSION_MINOR(instance_props.runtimeVersion);
        m_status.runtime_version_patch = XR_VERSION_PATCH(instance_props.runtimeVersion);

        XrSystemGetInfo system_info{ XR_TYPE_SYSTEM_GET_INFO };
        system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        result = xrGetSystem(m_instance, &system_info, &m_system_id);
        if (XR_FAILED(result))
        {
            m_status.hmd_available = false;
            setLastError("xrGetSystem(HMD) failed: " + resultToString(result));
            return false;
        }

        XrSystemProperties system_props{ XR_TYPE_SYSTEM_PROPERTIES };
        result = xrGetSystemProperties(m_instance, m_system_id, &system_props);
        if (XR_FAILED(result))
        {
            setLastError("xrGetSystemProperties failed: " + resultToString(result));
            return false;
        }

        m_status.hmd_available = true;
        m_status.system_name = system_props.systemName;
        return true;
    }

    void OpenXRSystem::queryViewConfiguration()
    {
        if (m_system_id == XR_NULL_SYSTEM_ID)
            return;

        uint32_t view_count = 0;
        XrResult result = xrEnumerateViewConfigurationViews(
            m_instance, m_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
        if (XR_FAILED(result) || view_count == 0)
        {
            setLastError("xrEnumerateViewConfigurationViews failed: " + resultToString(result));
            return;
        }

        std::vector<XrViewConfigurationView> xr_views(view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
        result = xrEnumerateViewConfigurationViews(
            m_instance, m_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            view_count, &view_count, xr_views.data());
        if (XR_FAILED(result))
        {
            setLastError("xrEnumerateViewConfigurationViews failed: " + resultToString(result));
            return;
        }

        m_views.clear();
        m_views.reserve(view_count);
        for (const XrViewConfigurationView& view : xr_views)
        {
            OpenXRViewInfo info;
            info.recommended_width = view.recommendedImageRectWidth;
            info.recommended_height = view.recommendedImageRectHeight;
            info.max_width = view.maxImageRectWidth;
            info.max_height = view.maxImageRectHeight;
            info.recommended_sample_count = view.recommendedSwapchainSampleCount;
            info.max_sample_count = view.maxSwapchainSampleCount;
            m_views.push_back(info);
        }

        m_status.view_count = static_cast<uint32_t>(m_views.size());
    }

    void OpenXRSystem::queryEnvironmentBlendMode()
    {
        if (m_system_id == XR_NULL_SYSTEM_ID)
            return;

        uint32_t mode_count = 0;
        XrResult result = xrEnumerateEnvironmentBlendModes(
            m_instance, m_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &mode_count, nullptr);
        if (XR_FAILED(result) || mode_count == 0)
            return;

        std::vector<XrEnvironmentBlendMode> modes(mode_count);
        result = xrEnumerateEnvironmentBlendModes(
            m_instance, m_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            mode_count, &mode_count, modes.data());
        if (XR_FAILED(result))
            return;

        auto opaque = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
        m_status.environment_blend_mode = opaque != modes.end() ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : modes.front();
    }

    bool OpenXRSystem::beginPIE(IRenderAPI* render_api, bool force_enable)
    {
        if (!force_enable && (!cvarEnabled("xr_enable", false) || !cvarEnabled("xr_pie", true)))
            return false;

        if (!initialize())
            return false;

        m_status.pie_active = true;
        m_status.backend_name = render_api ? render_api->getAPIName() : "Unknown";

        m_status.backend_supported = false;
        if (m_status.backend_name == "D3D12")
            m_status.backend_supported = isExtensionSupported(XR_EXT_D3D12);
        else if (m_status.backend_name == "Vulkan")
            m_status.backend_supported = isExtensionSupported(XR_EXT_VULKAN2) || isExtensionSupported(XR_EXT_VULKAN1);
        else if (m_status.backend_name == "Metal")
            m_status.backend_supported = isExtensionSupported(XR_EXT_METAL);

        if (!m_status.backend_supported)
        {
            setLastError("OpenXR runtime does not expose a graphics binding for " + m_status.backend_name);
            LOG_ENGINE_WARN("[OpenXR] PIE active but backend '{}' is not supported by the runtime", m_status.backend_name);
            return false;
        }

        LOG_ENGINE_INFO("[OpenXR] PIE runtime bridge active on {} ({}, blend mode {})",
                        m_status.backend_name,
                        m_status.system_name.empty() ? "no HMD system" : m_status.system_name.c_str(),
                        blendModeName(m_status.environment_blend_mode));
        return true;
    }

    void OpenXRSystem::endPIE()
    {
        if (!m_status.pie_active)
            return;

        m_status.pie_active = false;
        m_status.session_running = false;
        m_status.session_state = XR_SESSION_STATE_UNKNOWN;
        LOG_ENGINE_INFO("[OpenXR] PIE runtime bridge stopped");
    }

    void OpenXRSystem::pollEvents()
    {
        if (m_instance == XR_NULL_HANDLE)
            return;

        XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
        for (;;)
        {
            XrResult result = xrPollEvent(m_instance, &event);
            if (result == XR_EVENT_UNAVAILABLE)
                break;

            if (XR_FAILED(result))
            {
                setLastError("xrPollEvent failed: " + resultToString(result));
                break;
            }

            switch (event.type)
            {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            {
                m_status.instance_loss_pending = true;
                setLastError("OpenXR instance loss pending");
                LOG_ENGINE_WARN("[OpenXR] Instance loss pending");
                break;
            }
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            {
                const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                m_status.session_state = changed->state;
                m_status.session_running =
                    changed->state == XR_SESSION_STATE_SYNCHRONIZED ||
                    changed->state == XR_SESSION_STATE_VISIBLE ||
                    changed->state == XR_SESSION_STATE_FOCUSED;
                LOG_ENGINE_INFO("[OpenXR] Session state changed: {}", sessionStateName(changed->state));
                break;
            }
            default:
                break;
            }

            event = { XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    bool OpenXRSystem::isExtensionSupported(const char* extension_name) const
    {
        if (!extension_name)
            return false;

        return std::any_of(m_supported_extensions.begin(), m_supported_extensions.end(),
            [extension_name](const XrExtensionProperties& props)
            {
                return std::strcmp(props.extensionName, extension_name) == 0;
            });
    }

    std::string OpenXRSystem::resultToString(XrResult result) const
    {
        if (m_instance != XR_NULL_HANDLE)
        {
            std::array<char, XR_MAX_RESULT_STRING_SIZE> buffer{};
            if (XR_SUCCEEDED(xrResultToString(m_instance, result, buffer.data())))
                return buffer.data();
        }

        switch (result)
        {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
        case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
        case XR_EVENT_UNAVAILABLE: return "XR_EVENT_UNAVAILABLE";
        case XR_SPACE_BOUNDS_UNAVAILABLE: return "XR_SPACE_BOUNDS_UNAVAILABLE";
        case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
        case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
        case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
        case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
        default: return "XrResult(" + std::to_string(static_cast<int>(result)) + ")";
        }
    }

    void OpenXRSystem::setLastError(const std::string& message)
    {
        m_status.last_error = message;
    }
}
