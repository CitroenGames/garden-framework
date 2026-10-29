#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>

namespace fs = std::filesystem;

static std::string readText(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

static bool writeText(const fs::path& path, const char* text)
{
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    return static_cast<bool>(stream);
}

static size_t count(const std::string& value, const std::string& item)
{
    size_t occurrences = 0;
    for (size_t position = 0; (position = value.find(item, position)) != std::string::npos;
         position += item.size()) ++occurrences;
    return occurrences;
}

int main()
{
    const std::string name = "server process retains scene and module cleanup across retry";
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length == std::size(executable)) {
        std::cerr << "[FAIL] " << name << ": cannot resolve executable path\n";
        return 1;
    }
    const fs::path bin = fs::path(executable).parent_path();
    const fs::path server = bin / "Server.exe";
    const fs::path fixture = bin / "ServerHostLifecycleFixtures" / "server_retry.dll";
    const fs::path root = fs::temp_directory_path() /
        (L"garden-server-host-retry-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const fs::path project = root / "retry.garden";
    const fs::path recorder = root / "fixture.log";
    const fs::path output = root / "server.log";

    try {
        if (!fs::is_regular_file(server) || !fs::is_regular_file(fixture)) {
            std::cerr << "[FAIL] " << name << ": Server.exe or server_retry.dll missing\n";
            return 1;
        }
        fs::create_directories(root / "bin");
        fs::create_directories(root / "assets" / "levels");
        fs::copy_file(fixture, root / "bin" / "server_retry.dll");
        if (!writeText(project,
            R"({"name":"ServerHostLifecycleFixture","game_module":"bin/server_retry","asset_directories":["assets/"],"default_level":"assets/levels/retry.level.json","default_game_mode":"GameMode","default_game_state":"GameState"})") ||
            !writeText(root / "assets" / "levels" / "retry.level.json",
            R"({"metadata":{"level_name":"retry"},"entities":[{"name":"scene-owned","type":"Static"}]})")) {
            std::cerr << "[FAIL] " << name << ": cannot write fixture project\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << name << ": fixture setup: " << e.what() << '\n';
        return 1;
    }

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE log = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        std::cerr << "[FAIL] " << name << ": cannot open process handles\n";
        return 1;
    }
    wchar_t* old_recorder = nullptr;
    size_t old_size = 0;
    if (_wdupenv_s(&old_recorder, &old_size, L"GARDEN_SERVER_RETRY_RECORDER") != 0) {
        CloseHandle(input);
        CloseHandle(log);
        std::cerr << "[FAIL] " << name << ": cannot save recorder environment\n";
        return 1;
    }
    if (_wputenv_s(L"GARDEN_SERVER_RETRY_RECORDER", recorder.c_str()) != 0) {
        std::free(old_recorder);
        CloseHandle(input);
        CloseHandle(log);
        std::cerr << "[FAIL] " << name << ": cannot set recorder path\n";
        return 1;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + server.wstring() + L"\" --project \"" + project.wstring() + L"\"";
    const BOOL launched = CreateProcessW(server.c_str(), command.data(), nullptr, nullptr, TRUE,
                                         CREATE_NO_WINDOW, nullptr, bin.c_str(), &startup, &process);
    const DWORD launch_error = launched ? ERROR_SUCCESS : GetLastError();
    _wputenv_s(L"GARDEN_SERVER_RETRY_RECORDER", old_recorder ? old_recorder : L"");
    std::free(old_recorder);
    CloseHandle(input);
    CloseHandle(log);
    if (!launched) {
        std::cerr << "[FAIL] " << name << ": CreateProcess failed: " << launch_error << '\n';
        return 1;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD termination_wait = WAIT_OBJECT_0;
    if (wait != WAIT_OBJECT_0) {
        if (!TerminateProcess(process.hProcess, 124)) termination_wait = WAIT_FAILED;
        else termination_wait = WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);

    auto trace = readText(recorder);
    trace.erase(std::remove(trace.begin(), trace.end(), '\r'), trace.end());
    const auto log_text = readText(output);
    const std::vector<std::string> expected{
        "server.init\n", "server.quit.requested\n", "scene.fault.hierarchy\n", "scene.hook.removed\n",
        "server.shutdown\n", "scene.ack.observed\n", "fault.constraints\n",
        "fault.hook.removed\n"};
    size_t position = 0;
    bool ordered = true;
    for (const auto& marker : expected) {
        const auto found = trace.find(marker, position);
        if (found == std::string::npos || count(trace, marker) != 1) { ordered = false; break; }
        position = found + marker.size();
    }
    const bool ok = wait == WAIT_OBJECT_0 && exit_code == 0 && ordered &&
        log_text.find("Server started successfully") != std::string::npos &&
        log_text.find("Scene shutdown incomplete") != std::string::npos &&
        log_text.find("Game module retirement refused; retrying") != std::string::npos;
    if (!ok) {
        std::cerr << "[FAIL] " << name << ": wait=" << wait << ", exit=" << exit_code
                  << ", termination wait=" << termination_wait
                  << ", logs at " << root.string() << "\nTrace:\n" << trace
                  << "\nServer output:\n" << log_text << '\n';
        return 1;
    }
    std::cout << "[PASS] " << name << " (logs: " << root.string() << ")\n";
    return 0;
}
#else
int main()
{
    std::cout << "[SKIP] server process lifecycle fixture requires Windows\n";
    return 0;
}
#endif
