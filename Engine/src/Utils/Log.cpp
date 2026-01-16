#include "Log.hpp"

#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/callback_sink.h"
#include "spdlog/sinks/basic_file_sink.h"

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
