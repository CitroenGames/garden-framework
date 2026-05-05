#pragma once

#include "EngineExport.h"
#include "spdlog/spdlog.h"
#include <memory>
#include <type_traits>
#include <utility>

namespace EE
{
    // this class is a wrapper around the spdlog library
    class ENGINE_API CLog
    {
    public:
        static void Init();
        // this function should not be called before the engine is terminated
        static void Shutdown();

        static bool ShouldLogEngine(spdlog::level::level_enum level);
        static bool ShouldLogClient(spdlog::level::level_enum level);
        static bool ShouldLogLua(spdlog::level::level_enum level);

        static void LogEngineRaw(spdlog::level::level_enum level, const char* message, size_t length);
        static void LogClientRaw(spdlog::level::level_enum level, const char* message, size_t length);
        static void LogLuaRaw(spdlog::level::level_enum level, const char* message, size_t length);

        template <typename... Args>
        static void LogEngine(spdlog::level::level_enum level,
            spdlog::format_string_t<Args...> fmt_str, Args&&... args)
        {
            if (!ShouldLogEngine(level))
                return;

            auto formatted = spdlog::fmt_lib::format(fmt_str, std::forward<Args>(args)...);
            LogEngineRaw(level, formatted.data(), formatted.size());
        }

        static inline void LogEngine(spdlog::level::level_enum level, spdlog::string_view_t message)
        {
            if (!ShouldLogEngine(level))
                return;
            LogEngineRaw(level, message.data(), message.size());
        }

        template <typename T,
            typename std::enable_if<!spdlog::is_convertible_to_any_format_string<const T&>::value, int>::type = 0>
        static void LogEngine(spdlog::level::level_enum level, const T& message)
        {
            LogEngine(level, "{}", message);
        }

        template <typename... Args>
        static void LogClient(spdlog::level::level_enum level,
            spdlog::format_string_t<Args...> fmt_str, Args&&... args)
        {
            if (!ShouldLogClient(level))
                return;

            auto formatted = spdlog::fmt_lib::format(fmt_str, std::forward<Args>(args)...);
            LogClientRaw(level, formatted.data(), formatted.size());
        }

        static inline void LogClient(spdlog::level::level_enum level, spdlog::string_view_t message)
        {
            if (!ShouldLogClient(level))
                return;
            LogClientRaw(level, message.data(), message.size());
        }

        template <typename T,
            typename std::enable_if<!spdlog::is_convertible_to_any_format_string<const T&>::value, int>::type = 0>
        static void LogClient(spdlog::level::level_enum level, const T& message)
        {
            LogClient(level, "{}", message);
        }

        template <typename... Args>
        static void LogLua(spdlog::level::level_enum level,
            spdlog::format_string_t<Args...> fmt_str, Args&&... args)
        {
            if (!ShouldLogLua(level))
                return;

            auto formatted = spdlog::fmt_lib::format(fmt_str, std::forward<Args>(args)...);
            LogLuaRaw(level, formatted.data(), formatted.size());
        }

        static inline void LogLua(spdlog::level::level_enum level, spdlog::string_view_t message)
        {
            if (!ShouldLogLua(level))
                return;
            LogLuaRaw(level, message.data(), message.size());
        }

        template <typename T,
            typename std::enable_if<!spdlog::is_convertible_to_any_format_string<const T&>::value, int>::type = 0>
        static void LogLua(spdlog::level::level_enum level, const T& message)
        {
            LogLua(level, "{}", message);
        }

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

#define LOG_ENGINE_FATAL(...) ::EE::CLog::LogEngine(::spdlog::level::critical, __VA_ARGS__)
#define LOG_ENGINE_ERROR(...) ::EE::CLog::LogEngine(::spdlog::level::err, __VA_ARGS__)
#define LOG_ENGINE_WARN(...) ::EE::CLog::LogEngine(::spdlog::level::warn, __VA_ARGS__)
#define LOG_ENGINE_INFO(...) ::EE::CLog::LogEngine(::spdlog::level::info, __VA_ARGS__)
#define LOG_ENGINE_TRACE(...) ::EE::CLog::LogEngine(::spdlog::level::trace, __VA_ARGS__)

#define LOG_ERROR(...) ::EE::CLog::LogClient(::spdlog::level::err, __VA_ARGS__)
#define LOG_WARN(...) ::EE::CLog::LogClient(::spdlog::level::warn, __VA_ARGS__)
#define LOG_INFO(...) ::EE::CLog::LogClient(::spdlog::level::info, __VA_ARGS__)
#define LOG_TRACE(...) ::EE::CLog::LogClient(::spdlog::level::trace, __VA_ARGS__)

#define LOG_LUA_ERROR(...) ::EE::CLog::LogLua(::spdlog::level::err, __VA_ARGS__)
#define LOG_LUA_WARN(...) ::EE::CLog::LogLua(::spdlog::level::warn, __VA_ARGS__)
#define LOG_LUA_INFO(...) ::EE::CLog::LogLua(::spdlog::level::info, __VA_ARGS__)
#define LOG_LUA_TRACE(...) ::EE::CLog::LogLua(::spdlog::level::trace, __VA_ARGS__)
