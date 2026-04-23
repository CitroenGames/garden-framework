#include "EngineRegistry.hpp"
#include "ProjectFile.hpp"
#include "PluginFile.hpp"
#include "PathUtils.hpp"
#include "EnginePicker.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>

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

// ---- sighmake invocation (shared by cmdGenerate and cmdGeneratePlugin) ----

// Runs `sighmake <abs_buildscript> -D ENGINE_PATH=<engine_path>` from `cwd`
// and waits for completion. Returns 0 on success, non-zero on failure (the
// child's exit code or 1 for spawn failure). Mirrors the original inline
// block in cmdGenerate verbatim — extracted so plugin builds can reuse it
// without copy-paste drift.
static int runSighmake(const std::string& abs_buildscript,
                       const std::string& engine_path,
                       const std::string& cwd)
{
#ifdef _WIN32
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
        cwd.c_str(),
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
        return (int)exit_code;
    }
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0)
    {
        fprintf(stderr, "Error: fork() failed\n");
        return 1;
    }
    if (pid == 0)
    {
        if (chdir(cwd.c_str()) != 0)
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
        return code;
    }
    return 0;
#endif
}

// Compile a generated solution. sighmake only emits project files — for
// `garden generate-plugin` we want an end-to-end build so the editor finds
// the DLL the host expects.
//
// Windows: invoke msbuild via `cmd /c` so the VS Developer environment
//   variables (PATH includes msbuild.exe) are picked up. Falls back to the
//   raw `msbuild` if available without VsDevCmd.
// Unix: invoke `make` in the directory sighmake left build artifacts in.
//   Plugin buildscripts are generated with `-g makefile` for this path.
//
// Returns 0 on success, non-zero on failure (or if the toolchain is missing).
static int runMsbuild(const std::string& sln_path,
                      const std::string& configuration,
                      const std::string& platform)
{
#ifdef _WIN32
    // Quote the sln path; msbuild fails on paths with spaces otherwise.
    std::string cmdline = "msbuild \"" + sln_path +
                          "\" /nologo /m /verbosity:minimal" +
                          " /p:Configuration=" + configuration +
                          " /p:Platform=" + platform;
    printf("  Running: %s\n", cmdline.c_str());

    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(
        nullptr, const_cast<char*>(cmdline.c_str()),
        nullptr, nullptr, TRUE, 0, nullptr, nullptr,
        &si, &pi))
    {
        fprintf(stderr,
            "Error: Failed to launch msbuild (error %lu).\n"
            "  Run this command from a Visual Studio Developer Command Prompt,\n"
            "  or open the generated .sln in Visual Studio and build manually.\n",
            GetLastError());
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exit_code != 0)
    {
        fprintf(stderr, "\nError: msbuild failed (exit code %lu)\n", exit_code);
        return (int)exit_code;
    }
    return 0;
#else
    (void)sln_path; (void)configuration; (void)platform;
    // On Unix, we drive `make` from the sighmake build directory instead.
    // Caller is expected to invoke a different code path; signal "not done".
    return 1;
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

    if (runSighmake(abs_buildscript, engine_path, abs_project_dir) != 0)
        return 1;

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

// ---- Plugin commands ----

// Build an editor plugin from a .gardenplugin descriptor and deploy the
// manifest into <engine>/plugins/ next to the built DLL.
//
// Flow:
//   1. Parse `.gardenplugin`, validate required fields.
//   2. Resolve engine_id -> engine path (or prompt picker if unset).
//   3. Run sighmake with -D ENGINE_PATH=<engine_root> and CWD = plugin dir.
//   4. Copy the .gardenplugin into <engine>/plugins/<output_dll>.gardenplugin
//      so the editor's PluginManager finds the manifest at startup.
//   5. Verify <engine>/plugins/<output_dll>.<dllext> exists; warn otherwise.
static int cmdGeneratePlugin(const std::string& plugin_path)
{
    if (!fs::exists(plugin_path))
    {
        fprintf(stderr, "Error: '%s' does not exist.\n", plugin_path.c_str());
        return 1;
    }

    GardenPlugin plugin;
    if (!loadGardenPlugin(plugin_path, plugin))
        return 1;

    std::string plugin_dir = fs::absolute(plugin_path).parent_path().string();
    std::string buildscript_path = (fs::path(plugin_dir) / plugin.buildscript).string();
    if (!fs::exists(buildscript_path))
    {
        fprintf(stderr, "Error: buildscript '%s' (referenced by '%s') not found.\n",
                buildscript_path.c_str(), plugin_path.c_str());
        return 1;
    }

    EngineRegistry registry;

    // Resolve engine. If empty, fall back to picker so 'garden generate-plugin'
    // works on a fresh checkout where the .gardenplugin's engine_id is "".
    const EngineEntry* engine = nullptr;
    if (!plugin.engine_id.empty())
        engine = registry.findEngine(plugin.engine_id);

    if (!engine)
    {
        auto engines = registry.listEngines();
        if (engines.empty())
        {
            fprintf(stderr, "Error: No engines registered.\n");
            fprintf(stderr, "Run 'garden register-engine' from your engine directory.\n");
            return 1;
        }

        std::string picked = showEnginePicker(engines, plugin.name);
        if (picked.empty())
        {
            fprintf(stderr, "No engine selected.\n");
            return 1;
        }

        if (!setPluginEngineId(plugin_path, picked))
            return 1;

        plugin.engine_id = picked;
        engine = registry.findEngine(picked);
        if (!engine)
        {
            fprintf(stderr, "Error: Engine '%s' disappeared from registry.\n", picked.c_str());
            return 1;
        }
    }

    std::string engine_path = engine->path;
    for (char& c : engine_path) if (c == '\\') c = '/';

    std::string abs_buildscript = fs::absolute(buildscript_path).string();
    for (char& c : abs_buildscript) if (c == '\\') c = '/';

    printf("Building plugin '%s'...\n", plugin.name.c_str());
    printf("  Engine: %s\n", engine_path.c_str());
    printf("  Buildscript: %s\n", abs_buildscript.c_str());

    if (runSighmake(abs_buildscript, engine_path, plugin_dir) != 0)
        return 1;

    // sighmake only emits project files. Compile them so the editor finds a
    // DLL and not just a .slnx. Sighmake names the solution after the
    // [solution] block (which conventionally matches the plugin name).
#ifdef _WIN32
    fs::path sln_path = fs::path(engine->path) / "build" / (plugin.name + "_.slnx");
    if (!fs::exists(sln_path))
    {
        // Fall back to common alternate names; sighmake versions vary on
        // whether they append the trailing '_'.
        for (const std::string& candidate :
             { plugin.name + ".slnx", plugin.name + "_.sln", plugin.name + ".sln" })
        {
            fs::path p = fs::path(engine->path) / "build" / candidate;
            if (fs::exists(p)) { sln_path = p; break; }
        }
    }

    if (!fs::exists(sln_path))
    {
        fprintf(stderr,
            "Warning: Generated solution not found under '%s\\build\\'.\n"
            "  Open the .slnx in Visual Studio and build manually.\n",
            engine->path.c_str());
    }
    else
    {
        printf("\nBuilding plugin (Debug|x64) via msbuild...\n");
        if (runMsbuild(sln_path.string(), "Debug", "x64") != 0)
        {
            fprintf(stderr,
                "  Build failed. Open '%s' in Visual Studio for full diagnostics.\n",
                sln_path.string().c_str());
            return 1;
        }
    }
#else
    // Unix: sighmake should have been told `-g makefile` to produce a Makefile
    // we can drive. The current runSighmake doesn't pass -g, so on first cut
    // we just print guidance — the project workflow does the same.
    printf("\nNote: To compile, run `make` in the generated build directory.\n");
#endif

    // Deploy manifest next to the built DLL.
    fs::path engine_plugins_dir = fs::path(engine->path) / "plugins";
    std::error_code ec;
    fs::create_directories(engine_plugins_dir, ec);

    std::string output_stem = plugin.output_dll.empty() ? plugin.name : plugin.output_dll;
    fs::path deployed_manifest = engine_plugins_dir / (output_stem + ".gardenplugin");

    fs::copy_file(plugin_path, deployed_manifest,
                  fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        fprintf(stderr, "Warning: failed to copy manifest to '%s': %s\n",
                deployed_manifest.string().c_str(), ec.message().c_str());
    }
    else
    {
        printf("  Deployed manifest: %s\n", deployed_manifest.string().c_str());
    }

    // Verify the DLL is where we expect.
#ifdef _WIN32
    const std::string dll_ext = ".dll";
#elif defined(__APPLE__)
    const std::string dll_ext = ".dylib";
#else
    const std::string dll_ext = ".so";
#endif
    fs::path expected_dll = engine_plugins_dir / (output_stem + dll_ext);
    if (!fs::exists(expected_dll))
    {
        fprintf(stderr,
            "Warning: Expected DLL not found at '%s'.\n"
            "  Check that your .buildscript sets 'outdir = ${ENGINE_PATH}/plugins'\n"
            "  and 'name' matches output_dll ('%s').\n",
            expected_dll.string().c_str(), output_stem.c_str());
    }
    else
    {
        printf("  Built plugin: %s\n", expected_dll.string().c_str());
    }

    printf("\nPlugin '%s' built successfully.\n", plugin.name.c_str());
    return 0;
}

// Scaffold a new editor plugin from the EditorPlugin template. Copies the
// template directory to ./<name>/ and substitutes the placeholder name in
// .gardenplugin / .buildscript / source files.
//
// Usage: garden new-plugin <name> [--engine <id>]
static int cmdNewPlugin(int argc, char* argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: garden new-plugin <name> [--engine <id>]\n");
        return 1;
    }

    std::string name = argv[2];
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
    {
        fprintf(stderr, "Error: plugin name must be a single path component.\n");
        return 1;
    }

    std::string explicit_engine_id;
    for (int i = 3; i + 1 < argc; ++i)
    {
        if (std::string(argv[i]) == "--engine")
        {
            explicit_engine_id = argv[i + 1];
            ++i;
        }
    }

    EngineRegistry registry;

    // Pick the target engine.
    std::string engine_id = explicit_engine_id;
    const EngineEntry* engine = nullptr;
    if (!engine_id.empty())
    {
        engine = registry.findEngine(engine_id);
        if (!engine)
        {
            fprintf(stderr, "Error: Engine '%s' is not registered.\n", engine_id.c_str());
            return 1;
        }
    }
    else
    {
        auto engines = registry.listEngines();
        if (engines.empty())
        {
            fprintf(stderr, "Error: No engines registered.\n");
            fprintf(stderr, "Run 'garden register-engine' from your engine directory first.\n");
            return 1;
        }
        if (engines.size() == 1)
        {
            engine = &engines[0];
            engine_id = engine->id;
        }
        else
        {
            std::string picked = showEnginePicker(engines, name);
            if (picked.empty()) { fprintf(stderr, "No engine selected.\n"); return 1; }
            engine_id = picked;
            engine = registry.findEngine(picked);
        }
    }

    // Find template directory.
    fs::path template_dir = fs::path(engine->path) / "Templates" / "EditorPlugin";
    if (!fs::exists(template_dir))
    {
        fprintf(stderr, "Error: Template not found at '%s'.\n", template_dir.string().c_str());
        return 1;
    }

    fs::path target_dir = fs::current_path() / name;
    if (fs::exists(target_dir))
    {
        fprintf(stderr, "Error: '%s' already exists.\n", target_dir.string().c_str());
        return 1;
    }

    std::error_code ec;
    fs::copy(template_dir, target_dir, fs::copy_options::recursive, ec);
    if (ec)
    {
        fprintf(stderr, "Error: copy failed: %s\n", ec.message().c_str());
        return 1;
    }

    // Rename the .gardenplugin and .buildscript files to use the new name.
    auto rename_if_exists = [&](const std::string& old_stem, const std::string& ext) {
        fs::path old_path = target_dir / (old_stem + ext);
        if (fs::exists(old_path))
        {
            fs::path new_path = target_dir / (name + ext);
            fs::rename(old_path, new_path, ec);
        }
    };
    rename_if_exists("EditorPlugin", ".gardenplugin");
    rename_if_exists("EditorPlugin", ".buildscript");

    // Substitute "EditorPlugin" -> <name> inside .gardenplugin and .buildscript.
    auto substitute = [&](const fs::path& file) {
        if (!fs::exists(file)) return;
        std::ifstream in(file);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::string out_str;
        out_str.reserve(content.size());
        const std::string from = "EditorPlugin";
        size_t pos = 0;
        while (pos < content.size())
        {
            size_t hit = content.find(from, pos);
            if (hit == std::string::npos) { out_str.append(content, pos, std::string::npos); break; }
            out_str.append(content, pos, hit - pos);
            out_str.append(name);
            pos = hit + from.size();
        }
        std::ofstream out(file);
        out << out_str;
    };
    substitute(target_dir / (name + ".gardenplugin"));
    substitute(target_dir / (name + ".buildscript"));

    // Bake engine_id into the new .gardenplugin.
    setPluginEngineId((target_dir / (name + ".gardenplugin")).string(), engine_id);

    printf("Created plugin '%s' at %s\n", name.c_str(), target_dir.string().c_str());
    printf("\nNext steps:\n");
    printf("  cd %s\n", name.c_str());
    printf("  garden generate-plugin %s.gardenplugin\n", name.c_str());
    return 0;
}

// Enumerate plugins installed under each registered engine's plugins/ folder.
static int cmdListPlugins()
{
    EngineRegistry registry;
    auto engines = registry.listEngines();
    if (engines.empty())
    {
        printf("No engines registered.\n");
        return 0;
    }

    bool any = false;
    for (const auto& engine : engines)
    {
        fs::path plugins_dir = fs::path(engine.path) / "plugins";
        if (!fs::exists(plugins_dir)) continue;

        std::error_code ec;
        for (auto& entry : fs::directory_iterator(plugins_dir, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".gardenplugin") continue;

            GardenPlugin p;
            if (!loadGardenPlugin(entry.path().string(), p)) continue;

            if (!any)
            {
                printf("%-22s %-10s %-14s %-10s %s\n",
                       "NAME", "VERSION", "ENGINE", "STATUS", "MANIFEST");
                printf("%-22s %-10s %-14s %-10s %s\n",
                       "----", "-------", "------", "------", "--------");
                any = true;
            }

            printf("%-22s %-10s %-14s %-10s %s\n",
                   p.name.c_str(),
                   p.version.empty() ? "-" : p.version.c_str(),
                   engine.id.c_str(),
                   p.enabled ? "enabled" : "disabled",
                   entry.path().string().c_str());
        }
    }

    if (!any)
        printf("No plugins installed.\n");

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
    printf("\nPlugin commands:\n");
    printf("  generate-plugin <file.gardenplugin>  Build an editor plugin (runs sighmake + deploys manifest)\n");
    printf("  new-plugin <name> [--engine <id>]    Scaffold a new plugin from the EditorPlugin template\n");
    printf("  list-plugins                         List installed plugins across registered engines\n");
    printf("\n  --help, -h                        Show this help\n");
    printf("  --version, -v                     Show version\n");
    printf("\nFile association:\n");
    printf("  garden <file.garden>              Same as 'garden open <file.garden>'\n");
    printf("  garden <file.gardenplugin>        Same as 'garden generate-plugin <file.gardenplugin>'\n");
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

    // Plugin commands
    if (cmd == "generate-plugin" && argc >= 3)
    {
        int result = cmdGeneratePlugin(argv[2]);
#ifdef _WIN32
        printf("\n");
        system("pause");
#endif
        return result;
    }
    if (cmd == "new-plugin")
        return cmdNewPlugin(argc, argv);
    if (cmd == "list-plugins")
        return cmdListPlugins();

    // Implicit dispatch: .gardenplugin → generate-plugin (mirrors .garden → open).
    // Check this BEFORE .garden so the longer extension wins.
    if (cmd.size() > 13 && cmd.substr(cmd.size() - 13) == ".gardenplugin")
    {
        int result = cmdGeneratePlugin(cmd);
#ifdef _WIN32
        printf("\n");
        system("pause");
#endif
        return result;
    }

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
