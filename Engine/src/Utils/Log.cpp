#include "Log.hpp"
#include "Console/Console.hpp"

#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/callback_sink.h"
#include "spdlog/sinks/basic_file_sink.h"
#include <chrono>

#define EE_EngineName "Engine"
#define EE_AppName "Client"

namespace EE
{
	std::shared_ptr<spdlog::logger> CLog::S_EngineLogger;
	std::shared_ptr<spdlog::logger> CLog::S_ClientLogger;
	std::shared_ptr<spdlog::logger> CLog::S_LuaLogger;

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

		// make it output to file
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>( "Log.log", true );
        S_EngineLogger->sinks().push_back( file_sink );
        S_ClientLogger->sinks().push_back( file_sink );
		S_LuaLogger->sinks().push_back( file_sink );

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
