#include "EngineRegistry.hpp"
#include "ProjectFile.hpp"
#include "PathUtils.hpp"
#include "EnginePicker.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#else
#   include <unistd.h>
#   include <sys/wait.h>
#endif

namespace fs = std::filesystem;

// ---- Process launching ----

static bool launchEditor(const std::string& editor_path, const std::string& project_path)
{
    if (!fs::exists(editor_path))
    {
        fprintf(stderr, "Error: Editor not found at '%s'\n", editor_path.c_str());
        fprintf(stderr, "  Build the editor first, then re-register the engine.\n");
        return false;
    }

    std::string abs_project = fs::absolute(project_path).string();
    // Set working directory to the project directory so the editor can load project content
    std::string project_dir = fs::path(abs_project).parent_path().string();

#ifdef _WIN32
    // Build command line: "Editor.exe" --project "C:\path\to\project.garden"
    std::string cmdline = "\"" + editor_path + "\" --project \"" + abs_project + "\"";

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(
        nullptr,
        const_cast<char*>(cmdline.c_str()),
        nullptr, nullptr, FALSE,
        DETACHED_PROCESS, // Don't inherit console
        nullptr,
        project_dir.c_str(),
        &si, &pi))
    {
        fprintf(stderr, "Error: Failed to launch editor (error %lu)\n", GetLastError());
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "Error: fork() failed\n");
        return false;
    }
    if (pid == 0)
    {
        // Set working directory to project directory
        if (chdir(project_dir.c_str()) != 0)
            _exit(127);
        const char* args[] = {
            editor_path.c_str(),
            "--project",
            abs_project.c_str(),
            nullptr
        };
        execvp(args[0], const_cast<char* const*>(args));
        _exit(127); // exec failed
    }
    // Parent: don't wait — editor runs independently
    return true;
#endif
}

static bool launchClient(const std::string& client_path, const std::string& project_path)
{
    if (!fs::exists(client_path))
    {
        fprintf(stderr, "Error: Client not found at '%s'\n", client_path.c_str());
        fprintf(stderr, "  Build the client first, then try again.\n");
        return false;
    }

    std::string abs_project = fs::absolute(project_path).string();
    std::string project_dir = fs::path(abs_project).parent_path().string();

#ifdef _WIN32
    std::string cmdline = "\"" + client_path + "\" --project \"" + abs_project + "\"";

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(
        nullptr,
        const_cast<char*>(cmdline.c_str()),
        nullptr, nullptr, FALSE,
        DETACHED_PROCESS,
        nullptr,
        project_dir.c_str(),
        &si, &pi))
    {
        fprintf(stderr, "Error: Failed to launch client (error %lu)\n", GetLastError());
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "Error: fork() failed\n");
        return false;
    }
    if (pid == 0)
    {
        if (chdir(project_dir.c_str()) != 0)
            _exit(127);
        const char* args[] = {
            client_path.c_str(),
            "--project",
            abs_project.c_str(),
            nullptr
        };
        execvp(args[0], const_cast<char* const*>(args));
        _exit(127);
    }
    return true;
#endif
}

static bool launchServer(const std::string& server_path, const std::string& project_path)
{
    if (!fs::exists(server_path))
    {
        fprintf(stderr, "Error: Server not found at '%s'\n", server_path.c_str());
        fprintf(stderr, "  Build the server first, then try again.\n");
        return false;
    }

    std::string abs_project = fs::absolute(project_path).string();
    std::string project_dir = fs::path(abs_project).parent_path().string();

#ifdef _WIN32
    std::string cmdline = "\"" + server_path + "\" --project \"" + abs_project + "\"";

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(
        nullptr,
        const_cast<char*>(cmdline.c_str()),
        nullptr, nullptr, FALSE,
        DETACHED_PROCESS,
        nullptr,
        project_dir.c_str(),
        &si, &pi))
    {
        fprintf(stderr, "Error: Failed to launch server (error %lu)\n", GetLastError());
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "Error: fork() failed\n");
        return false;
    }
    if (pid == 0)
    {
        if (chdir(project_dir.c_str()) != 0)
            _exit(127);
        const char* args[] = {
            server_path.c_str(),
            "--project",
            abs_project.c_str(),
            nullptr
        };
        execvp(args[0], const_cast<char* const*>(args));
        _exit(127);
    }
    return true;
#endif
}

// ---- Commands ----

static int cmdRegisterEngine(int argc, char* argv[])
{
    std::string path = "."; // Default to current directory

    for (int i = 2; i < argc; i++)
    {
        if ((strcmp(argv[i], "--path") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc)
        {
            path = argv[++i];
        }
    }

    EngineRegistry registry;
    std::string id = registry.registerEngine(path);
    if (id.empty())
    {
        fprintf(stderr, "Failed to register engine.\n");
        return 1;
    }

    printf("Engine registered successfully.\n");
    printf("  ID:   %s\n", id.c_str());
    printf("  Path: %s\n", fs::canonical(path).string().c_str());
    printf("\nTo link a project, add this to your .garden file:\n");
    printf("  \"engine_id\": \"%s\"\n", id.c_str());
    printf("\nOr run: garden set-engine <project.garden> %s\n", id.c_str());
    return 0;
}

static int cmdUnregisterEngine(int argc, char* argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: garden unregister-engine <id>\n");
        return 1;
    }

    EngineRegistry registry;
    if (registry.unregisterEngine(argv[2]))
    {
        printf("Engine '%s' unregistered.\n", argv[2]);
        return 0;
    }
    return 1;
}

static int cmdListEngines()
{
    EngineRegistry registry;
    auto engines = registry.listEngines();

    if (engines.empty())
    {
        printf("No engines registered.\n");
        printf("Run 'garden register-engine' from your engine directory.\n");
        return 0;
    }

    printf("Registered engines:\n\n");
    int missing = 0;
    for (auto& e : engines)
    {
        printf("  ID:         %s\n", e.id.c_str());
        printf("  Path:       %s\n", e.path.c_str());
        printf("  Editor:     %s\n", e.editor.c_str());
        printf("  Version:    %s\n", e.version.c_str());
        printf("  Registered: %s\n", e.registered_at.c_str());
        if (!e.path_exists)
        {
            printf("  Status:     ** MISSING ** (directory not found)\n");
            missing++;
        }
        printf("\n");
    }
    if (missing > 0)
        printf("  %d engine(s) marked MISSING. Run 'garden clean-engines' to remove them.\n", missing);
    return 0;
}

static int cmdOpen(const std::string& garden_path)
{
    if (!fs::exists(garden_path))
    {
        fprintf(stderr, "Error: File not found: '%s'\n", garden_path.c_str());
        return 1;
    }

    GardenProject project;
    if (!loadGardenProject(garden_path, project))
        return 1;

    EngineRegistry registry;
    const EngineEntry* engine = nullptr;

    if (!project.engine_id.empty())
        engine = registry.findEngine(project.engine_id);

    if (engine && !engine->path_exists)
    {
        fprintf(stderr, "Warning: Engine '%s' directory no longer exists: %s\n",
                engine->id.c_str(), engine->path.c_str());
        fprintf(stderr, "  Please select a different engine.\n");
        engine = nullptr;
    }

    // If no engine_id set, or the ID wasn't found -> show picker
    if (!engine)
    {
        auto engines = registry.listEngines();
        std::string picked = showEnginePicker(engines, project.name);

        if (picked.empty())
        {
            fprintf(stderr, "No engine selected.\n");
            return 1;
        }

        // Write the chosen engine_id into the .garden file
        if (!setProjectEngineId(garden_path, picked))
            return 1;

        project.engine_id = picked;
        engine = registry.findEngine(picked);
        if (!engine)
        {
            fprintf(stderr, "Error: Engine '%s' disappeared from registry.\n", picked.c_str());
            return 1;
        }
    }

    // Re-resolve editor path at launch time (it may have been built after registration)
    fs::path resolved_editor = PathUtils::findEditorPath(engine->path);
    std::string editor_path = resolved_editor.empty() ? engine->editor : resolved_editor.string();

    printf("Opening '%s' with engine at '%s'...\n", project.name.c_str(), engine->path.c_str());

    if (!launchEditor(editor_path, garden_path))
        return 1;

    return 0;
}

static int cmdRun(const std::string& garden_path)
{
    if (!fs::exists(garden_path))
    {
        fprintf(stderr, "Error: File not found: '%s'\n", garden_path.c_str());
        return 1;
    }

    GardenProject project;
    if (!loadGardenProject(garden_path, project))
        return 1;

    EngineRegistry registry;
    const EngineEntry* engine = nullptr;

    if (!project.engine_id.empty())
        engine = registry.findEngine(project.engine_id);

    if (engine && !engine->path_exists)
    {
        fprintf(stderr, "Warning: Engine '%s' directory no longer exists: %s\n",
                engine->id.c_str(), engine->path.c_str());
        fprintf(stderr, "  Please select a different engine.\n");
        engine = nullptr;
    }

    if (!engine)
    {
        auto engines = registry.listEngines();
        std::string picked = showEnginePicker(engines, project.name);

        if (picked.empty())
        {
            fprintf(stderr, "No engine selected.\n");
            return 1;
        }

        if (!setProjectEngineId(garden_path, picked))
            return 1;

        project.engine_id = picked;
        engine = registry.findEngine(picked);
        if (!engine)
        {
            fprintf(stderr, "Error: Engine '%s' disappeared from registry.\n", picked.c_str());
            return 1;
        }
    }

    fs::path resolved_client = PathUtils::findClientPath(engine->path);
    if (resolved_client.empty())
    {
        fprintf(stderr, "Error: Client executable not found in engine at '%s'\n", engine->path.c_str());
        fprintf(stderr, "  Build the client first, then try again.\n");
        return 1;
    }

    printf("Running '%s' with engine at '%s'...\n", project.name.c_str(), engine->path.c_str());

    if (!launchClient(resolved_client.string(), garden_path))
        return 1;

    return 0;
}

static int cmdRunServer(const std::string& garden_path)
{
    if (!fs::exists(garden_path))
    {
        fprintf(stderr, "Error: File not found: '%s'\n", garden_path.c_str());
        return 1;
    }

    GardenProject project;
    if (!loadGardenProject(garden_path, project))
        return 1;

    EngineRegistry registry;
    const EngineEntry* engine = nullptr;

    if (!project.engine_id.empty())
        engine = registry.findEngine(project.engine_id);

    if (engine && !engine->path_exists)
    {
        fprintf(stderr, "Warning: Engine '%s' directory no longer exists: %s\n",
                engine->id.c_str(), engine->path.c_str());
        fprintf(stderr, "  Please select a different engine.\n");
        engine = nullptr;
    }

    if (!engine)
    {
        auto engines = registry.listEngines();
        std::string picked = showEnginePicker(engines, project.name);

        if (picked.empty())
        {
            fprintf(stderr, "No engine selected.\n");
            return 1;
        }

        if (!setProjectEngineId(garden_path, picked))
            return 1;

        project.engine_id = picked;
        engine = registry.findEngine(picked);
        if (!engine)
        {
            fprintf(stderr, "Error: Engine '%s' disappeared from registry.\n", picked.c_str());
            return 1;
        }
    }

    fs::path resolved_server = PathUtils::findServerPath(engine->path);
    if (resolved_server.empty())
    {
        fprintf(stderr, "Error: Server executable not found in engine at '%s'\n", engine->path.c_str());
        fprintf(stderr, "  Build the server first, then try again.\n");
        return 1;
    }

    printf("Running server for '%s' with engine at '%s'...\n", project.name.c_str(), engine->path.c_str());

    if (!launchServer(resolved_server.string(), garden_path))
        return 1;

    return 0;
}

static int cmdGenerate(const std::string& garden_path)
{
    if (!fs::exists(garden_path))
    {
        fprintf(stderr, "Error: File not found: '%s'\n", garden_path.c_str());
        return 1;
    }

    GardenProject project;
    if (!loadGardenProject(garden_path, project))
        return 1;

    if (project.buildscript.empty())
    {
        fprintf(stderr, "Error: No 'buildscript' field in '%s'\n", garden_path.c_str());
        fprintf(stderr, "  Add a \"buildscript\" field pointing to your project's .buildscript file.\n");
        return 1;
    }

    // Resolve buildscript path relative to the .garden file
    fs::path project_dir = fs::path(garden_path).parent_path();
    fs::path buildscript_path = project_dir / project.buildscript;

    if (!fs::exists(buildscript_path))
    {
        fprintf(stderr, "Error: Buildscript not found: '%s'\n", buildscript_path.string().c_str());
        return 1;
    }

    // Resolve engine path from registry
    EngineRegistry registry;
    const EngineEntry* engine = nullptr;

    if (!project.engine_id.empty())
        engine = registry.findEngine(project.engine_id);

    if (engine && !engine->path_exists)
    {
        fprintf(stderr, "Warning: Engine '%s' directory no longer exists: %s\n",
                engine->id.c_str(), engine->path.c_str());
        fprintf(stderr, "  Please select a different engine.\n");
        engine = nullptr;
    }

    if (!engine)
    {
        // Show picker if no engine_id or not found
        auto engines = registry.listEngines();
        std::string picked = showEnginePicker(engines, project.name);

        if (picked.empty())
        {
            fprintf(stderr, "No engine selected.\n");
            return 1;
        }

        if (!setProjectEngineId(garden_path, picked))
            return 1;

        project.engine_id = picked;
        engine = registry.findEngine(picked);
        if (!engine)
        {
            fprintf(stderr, "Error: Engine '%s' disappeared from registry.\n", picked.c_str());
            return 1;
        }
    }

    std::string engine_path = engine->path;

    // Normalize backslashes to forward slashes for sighmake
    for (char& c : engine_path)
        if (c == '\\') c = '/';

    std::string abs_buildscript = fs::absolute(buildscript_path).string();
    for (char& c : abs_buildscript)
        if (c == '\\') c = '/';

    std::string abs_project_dir = fs::absolute(project_dir.empty() ? "." : project_dir).string();

    printf("Generating project files for '%s'...\n", project.name.c_str());
    printf("  Engine: %s\n", engine_path.c_str());
    printf("  Buildscript: %s\n", abs_buildscript.c_str());

    // Find sighmake on PATH
#ifdef _WIN32
    // Build command line for CreateProcess
    std::string cmdline = "sighmake \"" + abs_buildscript + "\" -D ENGINE_PATH=" + engine_path;
    printf("  Running: %s\n\n", cmdline.c_str());

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(
        nullptr,
        const_cast<char*>(cmdline.c_str()),
        nullptr, nullptr, TRUE,
        0,
        nullptr,
        abs_project_dir.c_str(),
        &si, &pi))
    {
        fprintf(stderr, "Error: Failed to launch sighmake (error %lu)\n", GetLastError());
        fprintf(stderr, "  Make sure sighmake is installed and on your PATH.\n");
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exit_code != 0)
    {
        fprintf(stderr, "\nError: sighmake failed (exit code %lu)\n", exit_code);
        return 1;
    }
#else
    pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "Error: fork() failed\n");
        return 1;
    }
    if (pid == 0)
    {
        if (chdir(abs_project_dir.c_str()) != 0)
            _exit(127);
        std::string d_arg = "ENGINE_PATH=" + engine_path;
        const char* args[] = {
            "sighmake",
            abs_buildscript.c_str(),
            "-D",
            d_arg.c_str(),
            nullptr
        };
        execvp(args[0], const_cast<char* const*>(args));
        fprintf(stderr, "Error: Failed to run sighmake. Make sure it is installed and on your PATH.\n");
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        fprintf(stderr, "\nError: sighmake failed (exit code %d)\n", code);
        return 1;
    }
#endif

    printf("\nProject files generated successfully.\n");
    return 0;
}

static int cmdSetEngine(int argc, char* argv[])
{
    if (argc < 4)
    {
        fprintf(stderr, "Usage: garden set-engine <project.garden> <engine_id>\n");
        return 1;
    }

    std::string project_path = argv[2];
    std::string engine_id = argv[3];

    // Verify engine exists
    EngineRegistry registry;
    const EngineEntry* engine = registry.findEngine(engine_id);
    if (!engine)
    {
        fprintf(stderr, "Error: Engine ID '%s' not found.\n", engine_id.c_str());
        fprintf(stderr, "Run 'garden list-engines' to see available engines.\n");
        return 1;
    }

    if (!setProjectEngineId(project_path, engine_id))
        return 1;

    printf("Project '%s' linked to engine '%s' (%s)\n",
           project_path.c_str(), engine_id.c_str(), engine->path.c_str());
    return 0;
}

static int cmdCleanEngines()
{
    EngineRegistry registry;
    int removed = registry.removeStaleEngines();
    if (removed == 0)
        printf("All engines are valid. Nothing to clean.\n");
    else
        printf("Removed %d stale engine(s).\n", removed);
    return 0;
}

static int cmdChangeEngine(const std::string& garden_path)
{
    if (!fs::exists(garden_path))
    {
        fprintf(stderr, "Error: File not found: '%s'\n", garden_path.c_str());
        return 1;
    }

    GardenProject project;
    if (!loadGardenProject(garden_path, project))
        return 1;

    EngineRegistry registry;
    auto engines = registry.listEngines();

    if (engines.empty())
    {
        fprintf(stderr, "No engines registered.\n");
        fprintf(stderr, "Run 'garden register-engine' from your engine directory.\n");
        return 1;
    }

    printf("Current engine for '%s': %s\n", project.name.c_str(),
           project.engine_id.empty() ? "(none)" : project.engine_id.c_str());

    std::string picked = showEnginePicker(engines, project.name);

    if (picked.empty())
    {
        fprintf(stderr, "No engine selected.\n");
        return 1;
    }

    const EngineEntry* engine = registry.findEngine(picked);
    if (!engine)
    {
        fprintf(stderr, "Error: Engine '%s' disappeared from registry.\n", picked.c_str());
        return 1;
    }

    if (!setProjectEngineId(garden_path, picked))
        return 1;

    printf("Engine changed to '%s' (%s)\n", picked.c_str(), engine->path.c_str());
    return 0;
}

static void printHelp()
{
    printf("Garden Engine CLI\n\n");
    printf("Usage: garden <command> [options]\n\n");
    printf("Commands:\n");
    printf("  register-engine [--path <dir>]    Register an engine installation (default: cwd)\n");
    printf("  unregister-engine <id>            Remove an engine registration\n");
    printf("  list-engines                      List all registered engines\n");
    printf("  clean-engines                     Remove engines whose directories no longer exist\n");
    printf("  open <file.garden>                Open a project in its associated editor\n");
    printf("  run <file.garden>                 Run the game for a project\n");
    printf("  run-server <file.garden>          Run the server for a project\n");
    printf("  generate <file.garden>            Generate project files (runs sighmake)\n");
    printf("  set-engine <file.garden> <id>     Set the engine_id in a project file\n");
    printf("  change-engine <file.garden>       Pick a new engine for a project (GUI)\n");
    printf("  --help, -h                        Show this help\n");
    printf("  --version, -v                     Show version\n");
    printf("\nFile association:\n");
    printf("  garden <file.garden>              Same as 'garden open <file.garden>'\n");
    printf("\nConfig location:\n");
    printf("  %s\n", PathUtils::getEnginesJsonPath().string().c_str());
}

// ---- Entry point ----

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        printHelp();
        return 0;
    }

    std::string cmd = argv[1];

    // Help / version
    if (cmd == "--help" || cmd == "-h")
    {
        printHelp();
        return 0;
    }
    if (cmd == "--version" || cmd == "-v")
    {
        printf("Garden CLI v1.0\n");
        return 0;
    }

    // Subcommands
    if (cmd == "register-engine")
        return cmdRegisterEngine(argc, argv);
    if (cmd == "unregister-engine")
        return cmdUnregisterEngine(argc, argv);
    if (cmd == "list-engines")
        return cmdListEngines();
    if (cmd == "clean-engines")
        return cmdCleanEngines();
    if (cmd == "open" && argc >= 3)
        return cmdOpen(argv[2]);
    if (cmd == "run" && argc >= 3)
    {
        int result = cmdRun(argv[2]);
#ifdef _WIN32
        if (result != 0)
        {
            fprintf(stderr, "\n");
            system("pause");
        }
#endif
        return result;
    }
    if (cmd == "run-server" && argc >= 3)
    {
        int result = cmdRunServer(argv[2]);
#ifdef _WIN32
        if (result != 0)
        {
            fprintf(stderr, "\n");
            system("pause");
        }
#endif
        return result;
    }
    if (cmd == "generate" && argc >= 3)
    {
        int result = cmdGenerate(argv[2]);
#ifdef _WIN32
        printf("\n");
        system("pause");
#endif
        return result;
    }
    if (cmd == "set-engine")
        return cmdSetEngine(argc, argv);
    if (cmd == "change-engine" && argc >= 3)
        return cmdChangeEngine(argv[2]);

    // Implicit open: if argument ends with .garden, treat as 'open'
    if (cmd.size() > 7 && cmd.substr(cmd.size() - 7) == ".garden")
    {
        int result = cmdOpen(cmd);
#ifdef _WIN32
        if (result != 0)
        {
            // Pause so the user can read the error before the console window closes
            fprintf(stderr, "\n");
            system("pause");
        }
#endif
        return result;
    }

    fprintf(stderr, "Unknown command: '%s'\n", cmd.c_str());
    fprintf(stderr, "Run 'garden --help' for usage.\n");
    return 1;
}
