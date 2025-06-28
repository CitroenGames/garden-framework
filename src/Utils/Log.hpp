#pragma once

#include "spdlog/spdlog.h"
#include <memory>

namespace EE
{
    // this class is a wrapper around the spdlog library
    class CLog
    {
    public:
        static void Init();
        // this function should not be called before the engine is terminated
        static void Shutdown();

        static inline std::shared_ptr<spdlog::logger>& GetEngineLogger()
        {
            return S_EngineLogger;
        }

        static inline std::shared_ptr<spdlog::logger>& GetClientLogger()
        {
            return S_ClientLogger;
        }

        static inline std::shared_ptr<spdlog::logger>& GetLuaLogger()
        {
            return S_LuaLogger;
		}

    private:
        static std::shared_ptr<spdlog::logger> S_EngineLogger;
        static std::shared_ptr<spdlog::logger> S_ClientLogger;
        static std::shared_ptr<spdlog::logger> S_LuaLogger;
    };

} // namespace EE

#define LOG_ENGINE_FATAL( ... ) ::EE::CLog::GetEngineLogger()->critical(__VA_ARGS__)
#define LOG_ENGINE_ERROR(...) ::EE::CLog::GetEngineLogger()->error(__VA_ARGS__)
#define LOG_ENGINE_WARN(...) ::EE::CLog::GetEngineLogger()->warn(__VA_ARGS__)
#define LOG_ENGINE_INFO(...) ::EE::CLog::GetEngineLogger()->info(__VA_ARGS__)
#define LOG_ENGINE_TRACE(...) ::EE::CLog::GetEngineLogger()->trace(__VA_ARGS__)

#define LOG_ERROR(...) ::EE::CLog::GetClientLogger()->error(__VA_ARGS__)
#define LOG_WARN(...) ::EE::CLog::GetClientLogger()->warn(__VA_ARGS__)
#define LOG_INFO(...) ::EE::CLog::GetClientLogger()->info(__VA_ARGS__)
#define LOG_TRACE(...) ::EE::CLog::GetClientLogger()->trace(__VA_ARGS__)

#define LOG_LUA_ERROR(...) ::EE::CLog::GetLuaLogger()->error(__VA_ARGS__)
#define LOG_LUA_WARN(...) ::EE::CLog::GetLuaLogger()->warn(__VA_ARGS__)
#define LOG_LUA_INFO(...) ::EE::CLog::GetLuaLogger()->info(__VA_ARGS__)
#define LOG_LUA_TRACE(...) ::EE::CLog::GetLuaLogger()->trace(__VA_ARGS__)