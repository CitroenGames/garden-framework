#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <ShlObj.h>
#pragma comment(lib, "DbgHelp.lib")
#pragma comment(lib, "shell32.lib")
#elif defined(__APPLE__)
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <pwd.h>
#else // Linux and other Unix-like systems
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysinfo.h>
#include <pwd.h>
#endif

#include <fstream>
#include <string>
#include <chrono>
#include <thread>

constexpr int MAX_FRAMES = 20; // limit the number of frames for brevity

namespace Paingine2D {

    class CrashHandler {
    private:
        static CrashHandler* s_instance;
        std::string m_applicationName;
        std::string m_crashReportFolder;

        // Windows specific
#ifdef _WIN32
        LPTOP_LEVEL_EXCEPTION_FILTER m_previousFilter;
#endif

        // Unix (macOS/Linux) specific
#if defined(__APPLE__) || defined(__linux__)
        struct sigaction m_previousSignalActions[NSIG];
#endif

        // Private constructor for singleton
        CrashHandler() {}

    public:
        static CrashHandler* GetInstance() {
            if (!s_instance)
                s_instance = new CrashHandler();
            return s_instance;
        }

        // Get default crash folder path based on platform
        static std::string GetDefaultCrashFolder() {
            std::string path;

#ifdef _WIN32
            // Get Windows Documents folder
            char documentsPath[MAX_PATH];
            HRESULT result = SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, documentsPath);
            if (SUCCEEDED(result)) {
                path = std::string(documentsPath) + "\\Paingine\\Crashes\\";
            }
            else {
                // Fallback to current directory if Documents folder can't be retrieved
                path = ".\\Crashes\\";
            }
#elif defined(__APPLE__)
            // macOS home directory + Documents
            const char* homeDir = getenv("HOME");
            if (homeDir) {
                path = std::string(homeDir) + "/Documents/Paingine/Crashes/";
            }
            else {
                struct passwd* pw = getpwuid(getuid());
                path = std::string(pw->pw_dir) + "/Documents/Paingine/Crashes/";
            }
#else // Linux
            // Linux home directory
            const char* homeDir = getenv("HOME");
            if (homeDir) {
                path = std::string(homeDir) + "/.paingine/crashes/";
            }
            else {
                struct passwd* pw = getpwuid(getuid());
                path = std::string(pw->pw_dir) + "/.paingine/crashes/";
            }
#endif

            return path;
        }

        bool Initialize(const std::string& applicationName, const std::string& crashReportFolder = "") {
            m_applicationName = applicationName;
            m_crashReportFolder = crashReportFolder;

            // If no folder specified, use default location
            if (m_crashReportFolder.empty()) {
                m_crashReportFolder = GetDefaultCrashFolder();
            }

            // Create crash report directory
#ifdef _WIN32
            if (SHCreateDirectoryExA(NULL, m_crashReportFolder.c_str(), NULL) != ERROR_SUCCESS &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                // Handle error if needed
            }
#else
            std::string mkdirCmd = "mkdir -p \"" + m_crashReportFolder + "\"";
            system(mkdirCmd.c_str());
#endif

            // Set up platform-specific exception handlers
#ifdef _WIN32
            m_previousFilter = SetUnhandledExceptionFilter(WindowsExceptionHandler);
#elif defined(__APPLE__) || defined(__linux__)
            InstallSignalHandlers();
#endif

            return true;
        }

        void Shutdown() {
            // Restore previous exception/signal handlers
#ifdef _WIN32
            SetUnhandledExceptionFilter(m_previousFilter);
#elif defined(__APPLE__) || defined(__linux__)
            RestoreSignalHandlers();
#endif
        }

    private:
        std::string GenerateCrashReportPath() {
            // Generate a filename with timestamp
            time_t now = time(0);
            tm* timeinfo = localtime(&now);

            char timestamp[80];
            strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", timeinfo);

            std::stringstream ss;
            ss << m_crashReportFolder << m_applicationName << "_" << timestamp;

            return ss.str();
        }

        void WriteSystemInfo(std::ofstream& logFile) {
            logFile << "======= Crash Report for " << m_applicationName << " =======" << std::endl;
            logFile << "Time: " << time(nullptr) << std::endl;

#ifdef _WIN32
            // Windows system info
            SYSTEM_INFO sysInfo;
            GetSystemInfo(&sysInfo);

            logFile << "Platform: Windows" << std::endl;
            logFile << "Processor count: " << sysInfo.dwNumberOfProcessors << std::endl;

            // Memory info
            MEMORYSTATUSEX memInfo;
            memInfo.dwLength = sizeof(MEMORYSTATUSEX);
            if (GlobalMemoryStatusEx(&memInfo)) {
                logFile << "Memory load: " << memInfo.dwMemoryLoad << "%" << std::endl;
                logFile << "Total physical memory: " << (memInfo.ullTotalPhys / (1024 * 1024)) << " MB" << std::endl;
                logFile << "Available physical memory: " << (memInfo.ullAvailPhys / (1024 * 1024)) << " MB" << std::endl;
            }

            // OS Version info
            OSVERSIONINFOEXA osvi;
            ZeroMemory(&osvi, sizeof(OSVERSIONINFOEXA));
            osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEXA);

#pragma warning(disable: 4996) // GetVersionEx is deprecated
            if (GetVersionExA((OSVERSIONINFOA*)&osvi)) {
                logFile << "OS Version: " << osvi.dwMajorVersion << "." << osvi.dwMinorVersion << " Build " << osvi.dwBuildNumber << std::endl;
            }
#elif defined(__APPLE__)
            // macOS system info
            logFile << "Platform: macOS" << std::endl;

            // Get processor count
            int mib[2];
            int ncpu;
            size_t len = sizeof(ncpu);
            mib[0] = CTL_HW;
            mib[1] = HW_NCPU;
            if (sysctl(mib, 2, &ncpu, &len, NULL, 0) != -1) {
                logFile << "Processor count: " << ncpu << std::endl;
            }

            // Get memory info
            mach_port_t host_port = mach_host_self();
            vm_size_t page_size;
            host_page_size(host_port, &page_size);

            vm_statistics_data_t vm_stats;
            mach_msg_type_number_t count = sizeof(vm_stats) / sizeof(natural_t);
            if (host_statistics(host_port, HOST_VM_INFO, (host_info_t)&vm_stats, &count) == KERN_SUCCESS) {
                uint64_t free_memory = (uint64_t)vm_stats.free_count * (uint64_t)page_size;
                uint64_t used_memory = ((uint64_t)vm_stats.active_count +
                    (uint64_t)vm_stats.inactive_count +
                    (uint64_t)vm_stats.wire_count) * (uint64_t)page_size;
                logFile << "Free memory: " << (free_memory / (1024 * 1024)) << " MB" << std::endl;
                logFile << "Used memory: " << (used_memory / (1024 * 1024)) << " MB" << std::endl;
            }

            // Get OS version
            char str[256];
            size_t size = sizeof(str);
            if (sysctlbyname("kern.osrelease", str, &size, NULL, 0) == 0) {
                logFile << "OS Release: " << str << std::endl;
            }
#else // Linux
            // Linux system info
            logFile << "Platform: Linux" << std::endl;

            // Get processor count
            logFile << "Processor count: " << sysconf(_SC_NPROCESSORS_ONLN) << std::endl;

            // Get memory info
            struct sysinfo memInfo;
            if (sysinfo(&memInfo) == 0) {
                logFile << "Total memory: " << (memInfo.totalram * memInfo.mem_unit / (1024 * 1024)) << " MB" << std::endl;
                logFile << "Free memory: " << (memInfo.freeram * memInfo.mem_unit / (1024 * 1024)) << " MB" << std::endl;
                logFile << "Uptime: " << (memInfo.uptime / 3600) << " hours" << std::endl;
            }

            // Try to get Linux distribution info
            std::ifstream osReleaseFile("/etc/os-release");
            if (osReleaseFile.is_open()) {
                std::string line;
                while (std::getline(osReleaseFile, line)) {
                    if (line.find("PRETTY_NAME=") == 0) {
                        logFile << "Distribution: " << line.substr(13, line.length() - 14) << std::endl;
                        break;
                    }
                }
                osReleaseFile.close();
            }
#endif
        }

#ifdef _WIN32
        // Windows-specific functions

        bool WriteMiniDump(EXCEPTION_POINTERS* exceptionPointers, const std::string& filePath) {
            // Open the file
            HANDLE hFile = CreateFileA(
                (filePath + ".dmp").c_str(),
                GENERIC_WRITE,
                0,
                NULL,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                NULL
            );

            if (hFile == INVALID_HANDLE_VALUE)
                return false;

            // Write the minidump
            MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
            exceptionInfo.ThreadId = GetCurrentThreadId();
            exceptionInfo.ExceptionPointers = exceptionPointers;
            exceptionInfo.ClientPointers = FALSE;

            BOOL result = MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                hFile,
                MiniDumpWithFullMemory, // More comprehensive dump
                &exceptionInfo,
                NULL,
                NULL
            );

            CloseHandle(hFile);
            return (result == TRUE);
        }

        void WriteLogFile(EXCEPTION_POINTERS* exceptionPointers, const std::string& crashReportPath) {
            // Create a log file alongside the minidump
            std::string logPath = crashReportPath + ".log";
            std::ofstream logFile(logPath);

            if (logFile.is_open()) {
                // Write system info
                WriteSystemInfo(logFile);

                // Exception specific info
                if (exceptionPointers) {
                    EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
                    logFile << "Exception code: 0x" << std::hex << record->ExceptionCode << std::dec << std::endl;
                    logFile << "Exception address: 0x" << std::hex << record->ExceptionAddress << std::dec << std::endl;

                    // Extract and write stack trace using StackWalk64
                    WriteWindowsStackTrace(logFile, exceptionPointers);
                }

                logFile.close();
            }
        }

        // Extracts and writes the stack trace using StackWalk64.
        static void WriteWindowsStackTrace(std::ofstream& logFile, EXCEPTION_POINTERS* exceptionPointers) {
            HANDLE process = GetCurrentProcess();
            HANDLE thread = GetCurrentThread();

            if (!SymInitialize(process, NULL, TRUE)) {
                logFile << "SymInitialize failed with error: " << GetLastError() << std::endl;
                return;
            }

            CONTEXT context = *exceptionPointers->ContextRecord;
            STACKFRAME64 stackFrame;
            memset(&stackFrame, 0, sizeof(STACKFRAME64));

#if defined(_M_IX86)
            DWORD machineType = IMAGE_FILE_MACHINE_I386;
            stackFrame.AddrPC.Offset = context.Eip;
            stackFrame.AddrPC.Mode = AddrModeFlat;
            stackFrame.AddrFrame.Offset = context.Ebp;
            stackFrame.AddrFrame.Mode = AddrModeFlat;
            stackFrame.AddrStack.Offset = context.Esp;
            stackFrame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_X64)
            DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
            stackFrame.AddrPC.Offset = context.Rip;
            stackFrame.AddrPC.Mode = AddrModeFlat;
            stackFrame.AddrFrame.Offset = context.Rsp;
            stackFrame.AddrFrame.Mode = AddrModeFlat;
            stackFrame.AddrStack.Offset = context.Rsp;
            stackFrame.AddrStack.Mode = AddrModeFlat;
#else
            DWORD machineType = 0;
#endif

            logFile << "Stack trace:" << std::endl;
            for (int frame = 0; frame < MAX_FRAMES; ++frame) {
                BOOL result = StackWalk64(
                    machineType,
                    process,
                    thread,
                    &stackFrame,
                    &context,
                    NULL,
                    SymFunctionTableAccess64,
                    SymGetModuleBase64,
                    NULL
                );
                if (!result || stackFrame.AddrPC.Offset == 0)
                    break;

                DWORD64 address = stackFrame.AddrPC.Offset;

                // Prepare symbol info buffer
                char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
                memset(buffer, 0, sizeof(buffer));
                PSYMBOL_INFO symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = MAX_SYM_NAME;

                if (SymFromAddr(process, address, 0, symbol)) {
                    logFile << "  " << frame << ": " << symbol->Name;
                    // Attempt to get source file and line info
                    IMAGEHLP_LINE64 line;
                    memset(&line, 0, sizeof(IMAGEHLP_LINE64));
                    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                    DWORD displacement = 0;
                    if (SymGetLineFromAddr64(process, address, &displacement, &line)) {
                        logFile << " (" << line.FileName << ":" << line.LineNumber << ")";
                    }
                    logFile << " - 0x" << std::hex << symbol->Address << std::dec << std::endl;
                }
                else {
                    logFile << "  " << frame << ": ??? - 0x" << std::hex << address << std::dec << std::endl;
                }
            }
            SymCleanup(process);
        }

        static LONG WINAPI WindowsExceptionHandler(EXCEPTION_POINTERS* exceptionPointers) {
            CrashHandler* handler = CrashHandler::GetInstance();

            // Generate the crash report path
            std::string crashReportPath = handler->GenerateCrashReportPath();

            // Write the minidump
            bool dumpWritten = handler->WriteMiniDump(exceptionPointers, crashReportPath);

            // Write additional log info
            if (dumpWritten) {
                handler->WriteLogFile(exceptionPointers, crashReportPath);
            }

            // Show crash dialog (just console output for now)
            //std::cerr << "The application has crashed. A crash report has been saved to: "
            //    << crashReportPath << ".dmp" << std::endl;

            // If you had a previous exception filter, you could call it here
            if (handler->m_previousFilter)
                return handler->m_previousFilter(exceptionPointers);

            return EXCEPTION_EXECUTE_HANDLER;
        }
#elif defined(__APPLE__) || defined(__linux__)
        // Unix (macOS/Linux) specific functions
        void InstallSignalHandlers() {
            struct sigaction action;
            memset(&action, 0, sizeof(action));
            action.sa_sigaction = UnixSignalHandler;
            action.sa_flags = SA_SIGINFO;

            // Install for various crash signals
            sigaction(SIGSEGV, &action, &m_previousSignalActions[SIGSEGV]);
            sigaction(SIGABRT, &action, &m_previousSignalActions[SIGABRT]);
            sigaction(SIGFPE, &action, &m_previousSignalActions[SIGFPE]);
            sigaction(SIGILL, &action, &m_previousSignalActions[SIGILL]);
            sigaction(SIGBUS, &action, &m_previousSignalActions[SIGBUS]);
        }

        void RestoreSignalHandlers() {
            sigaction(SIGSEGV, &m_previousSignalActions[SIGSEGV], NULL);
            sigaction(SIGABRT, &m_previousSignalActions[SIGABRT], NULL);
            sigaction(SIGFPE, &m_previousSignalActions[SIGFPE], NULL);
            sigaction(SIGILL, &m_previousSignalActions[SIGILL], NULL);
            sigaction(SIGBUS, &m_previousSignalActions[SIGBUS], NULL);
        }

        void WriteStackTrace(std::ofstream& logFile) {
            const int MAX_STACK_FRAMES = 64;
            void* stack[MAX_STACK_FRAMES];

            int frames = backtrace(stack, MAX_STACK_FRAMES);
            char** symbols = backtrace_symbols(stack, frames);

            logFile << "Stack trace:" << std::endl;

            if (symbols) {
                for (int i = 0; i < frames; i++) {
                    logFile << "  " << symbols[i] << std::endl;
                }
                free(symbols);
            }
            else {
                logFile << "  (Failed to obtain stack trace)" << std::endl;
            }
        }

        void WriteUnixCrashReport(int signal, siginfo_t* info, const std::string& crashReportPath) {
            std::string logPath = crashReportPath + ".log";
            std::ofstream logFile(logPath);

            if (logFile.is_open()) {
                // Write system info
                WriteSystemInfo(logFile);

                // Signal specific info
                logFile << "Signal: " << signal << " (" << strsignal(signal) << ")" << std::endl;
                if (info) {
                    logFile << "Signal code: " << info->si_code << std::endl;
                    logFile << "Fault address: " << info->si_addr << std::endl;
                }

                // Write stack trace
                WriteStackTrace(logFile);

                logFile.close();
            }
        }

        static void UnixSignalHandler(int signal, siginfo_t* info, void* context) {
            CrashHandler* handler = CrashHandler::GetInstance();

            // Generate the crash report path
            std::string crashReportPath = handler->GenerateCrashReportPath();

            // Write crash report
            handler->WriteUnixCrashReport(signal, info, crashReportPath);

            // Show crash info
            std::cerr << "The application has crashed. A crash report has been saved to: "
                << crashReportPath << ".log" << std::endl;

            // Restore default handler and re-raise signal
            signal(signal, SIG_DFL);
            raise(signal);
        }
#endif
    };

    // Initialize static member
    CrashHandler* CrashHandler::s_instance = nullptr;

} // namespace Paingine
