#include "Log.hpp"
#include "Console/Console.hpp"
#include "Utils/EnginePaths.hpp"

#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/callback_sink.h"
#include "spdlog/sinks/basic_file_sink.h"
#include <chrono>
#include <cstdio>

#define EE_EngineName "Engine"
#define EE_AppName "Client"

namespace EE
{
	std::shared_ptr<spdlog::logger> CLog::S_EngineLogger;
	std::shared_ptr<spdlog::logger> CLog::S_ClientLogger;
	std::shared_ptr<spdlog::logger> CLog::S_LuaLogger;

	namespace
	{
		bool shouldLog(const std::shared_ptr<spdlog::logger>& logger, spdlog::level::level_enum level)
		{
			return logger && logger->should_log(level);
		}

		void logRaw(const std::shared_ptr<spdlog::logger>& logger,
			spdlog::level::level_enum level, const char* message, size_t length)
		{
			if (!logger)
				return;

			if (!message)
			{
				message = "";
				length = 0;
			}

			logger->log(level, spdlog::string_view_t(message, length));
		}
	}

	void CLog::Init()
	{
		spdlog::set_pattern( "[%H:%M:%S %z] [%n] [%^---%L---%$] [thread %t] %v" );
		auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

		S_EngineLogger = std::make_shared<spdlog::logger>( EE_EngineName, spdlog::sinks_init_list { console_sink} );
        S_EngineLogger->set_level( spdlog::level::trace );

		// initialize the client logger
        S_ClientLogger = std::make_shared<spdlog::logger>( EE_AppName, spdlog::sinks_init_list { console_sink } );
		S_ClientLogger->set_level(spdlog::level::trace);

		// initialize the lua logger
        S_LuaLogger = std::make_shared<spdlog::logger>( "LUA", spdlog::sinks_init_list { console_sink } );
		S_LuaLogger->set_level(spdlog::level::trace);

		// Keep logs beside the executable rather than in the process working directory,
		// which may belong to a launcher or be read-only (for example, WindowsApps).
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> file_sink;
        try
        {
            const auto log_path = EnginePaths::getExecutableDir() / "Log.log";
            file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path.string(), true);
        }
        catch (const spdlog::spdlog_ex& e)
        {
            // File logging is optional; retain stdout and the console callback sinks.
            std::fprintf(stderr, "[CLog] Could not open log file beside the executable: %s\n", e.what());
        }

        if (file_sink)
        {
            S_EngineLogger->sinks().push_back(file_sink);
            S_ClientLogger->sinks().push_back(file_sink);
			S_LuaLogger->sinks().push_back(file_sink);
        }

		// Add callback sink to forward logs to Console
		auto console_callback = std::make_shared<spdlog::sinks::callback_sink_mt>(
			[](const spdlog::details::log_msg& msg) {
				ConsoleLogEntry entry;
				entry.message = std::string(msg.payload.begin(), msg.payload.end());
				entry.level = msg.level;
				entry.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
					msg.time.time_since_epoch()).count();
				entry.source = std::string(msg.logger_name.begin(), msg.logger_name.end());
				Console::get().addLogEntry(entry);
			}
		);
		S_EngineLogger->sinks().push_back(console_callback);
        S_ClientLogger->sinks().push_back(console_callback);
		S_LuaLogger->sinks().push_back(console_callback);
    }

	bool CLog::ShouldLogEngine(spdlog::level::level_enum level)
	{
		return shouldLog(S_EngineLogger, level);
	}

	bool CLog::ShouldLogClient(spdlog::level::level_enum level)
	{
		return shouldLog(S_ClientLogger, level);
	}

	bool CLog::ShouldLogLua(spdlog::level::level_enum level)
	{
		return shouldLog(S_LuaLogger, level);
	}

	void CLog::LogEngineRaw(spdlog::level::level_enum level, const char* message, size_t length)
	{
		logRaw(S_EngineLogger, level, message, length);
	}

	void CLog::LogClientRaw(spdlog::level::level_enum level, const char* message, size_t length)
	{
		logRaw(S_ClientLogger, level, message, length);
	}

	void CLog::LogLuaRaw(spdlog::level::level_enum level, const char* message, size_t length)
	{
		logRaw(S_LuaLogger, level, message, length);
	}

	void CLog::Shutdown()
	{
		LOG_ENGINE_TRACE( "Destroying Log" );
		// wait for the async logger to finish (if there is any)
		spdlog::drop_all();

		// shutdown all loggers
		spdlog::shutdown();
        S_EngineLogger.reset();
		S_ClientLogger.reset();
		S_LuaLogger.reset();
    }
}
