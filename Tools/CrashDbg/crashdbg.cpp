// crashdbg - minimal Windows crash debugger.
//
// Launches a program under the debugger, resolves symbols from its PDBs and
// prints a symbolized call stack when the process takes a fatal exception.
// Also forwards OutputDebugString output (D3D12 debug layer messages land there).
//
// Usage: crashdbg [-cwd <dir>] [-nodebugstr] <exe> [args...]
// Build: Tools\CrashDbg\build.bat

#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <map>
#include <string>

#pragma comment(lib, "dbghelp.lib")

static HANDLE g_process = nullptr;
static std::map<DWORD, HANDLE> g_threads;

static std::string finalPath(HANDLE file)
{
    if (!file) return {};
    char buf[MAX_PATH * 2] = {};
    DWORD n = GetFinalPathNameByHandleA(file, buf, sizeof(buf), VOLUME_NAME_DOS);
    if (n == 0 || n >= sizeof(buf)) return {};
    std::string p(buf);
    if (p.rfind("\\\\?\\", 0) == 0) p = p.substr(4);
    return p;
}

static void loadModule(HANDLE file, LPVOID base)
{
    std::string path = finalPath(file);
    if (path.empty()) return;
    SymLoadModuleEx(g_process, nullptr, path.c_str(), nullptr, (DWORD64)base, 0, nullptr, 0);
}

static void printFrame(int index, DWORD64 addr)
{
    char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;

    IMAGEHLP_MODULE64 mod = {};
    mod.SizeOfStruct = sizeof(mod);
    const char* modName = SymGetModuleInfo64(g_process, addr, &mod) ? mod.ModuleName : "?";

    DWORD64 disp = 0;
    if (SymFromAddr(g_process, addr, &disp, sym))
    {
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(g_process, addr, &lineDisp, &line))
            printf("  #%-2d %s!%s+0x%llx  (%s:%lu)\n", index, modName, sym->Name, disp, line.FileName, line.LineNumber);
        else
            printf("  #%-2d %s!%s+0x%llx\n", index, modName, sym->Name, disp);
    }
    else
    {
        printf("  #%-2d %s!0x%llx\n", index, modName, addr);
    }
}

static void printStack(DWORD threadId)
{
    HANDLE thread = g_threads.count(threadId) ? g_threads[threadId] : nullptr;
    if (!thread) return;

    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) return;

    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = ctx.Rip;      frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;   frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;   frame.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < 48; i++)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_process, thread, &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0) break;
        printFrame(i, frame.AddrPC.Offset);
    }
}

static const char* exceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:     return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:       return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:   return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:  return "ILLEGAL_INSTRUCTION";
    case 0xC0000374:                     return "HEAP_CORRUPTION";
    case 0xC0000409:                     return "FAST_FAIL (stack buffer / abort)";
    default:                             return "EXCEPTION";
    }
}

static bool isFatal(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW
        || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION
        || code == 0xC0000374 || code == 0xC0000409 || code == 0xC0000005;
}

int main(int argc, char** argv)
{
    std::string cwd;
    bool debugStrings = true;
    int i = 1;
    for (; i < argc; i++)
    {
        if (!strcmp(argv[i], "-cwd") && i + 1 < argc) cwd = argv[++i];
        else if (!strcmp(argv[i], "-nodebugstr")) debugStrings = false;
        else break;
    }
    if (i >= argc)
    {
        fprintf(stderr, "usage: crashdbg [-cwd <dir>] [-nodebugstr] <exe> [args...]\n");
        return 2;
    }

    std::string exe = argv[i];
    std::string cmd;
    for (int a = i; a < argc; a++)
    {
        if (a > i) cmd += ' ';
        cmd += '"'; cmd += argv[a]; cmd += '"';
    }

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe.c_str(), &cmd[0],nullptr, nullptr, FALSE, DEBUG_ONLY_THIS_PROCESS,
                        nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
    {
        fprintf(stderr, "CreateProcess failed (%lu)\n", GetLastError());
        return 2;
    }
    g_process = pi.hProcess;

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    std::string exeDir = exe.substr(0, exe.find_last_of("\\/"));
    SymInitialize(g_process, exeDir.c_str(), FALSE);
    printf("[crashdbg] debugging pid %lu\n", pi.dwProcessId);
    fflush(stdout);

    DWORD exitCode = 0;
    bool running = true;
    while (running)
    {
        DEBUG_EVENT ev = {};
        if (!WaitForDebugEvent(&ev, INFINITE)) break;
        DWORD cont = DBG_CONTINUE;

        switch (ev.dwDebugEventCode)
        {
        case CREATE_PROCESS_DEBUG_EVENT:
            g_threads[ev.dwThreadId] = ev.u.CreateProcessInfo.hThread;
            loadModule(ev.u.CreateProcessInfo.hFile, ev.u.CreateProcessInfo.lpBaseOfImage);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            g_threads[ev.dwThreadId] = ev.u.CreateThread.hThread;
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            g_threads.erase(ev.dwThreadId);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            loadModule(ev.u.LoadDll.hFile, ev.u.LoadDll.lpBaseOfDll);
            break;
        case OUTPUT_DEBUG_STRING_EVENT:
            if (debugStrings)
            {
                auto& d = ev.u.DebugString;
                std::string s(d.nDebugStringLength, '\0');
                SIZE_T read = 0;
                if (ReadProcessMemory(g_process, d.lpDebugStringData, &s[0], s.size(), &read))
                {
                    s.resize(strnlen(s.c_str(), read));
                    printf("[dbgstr] %s%s", s.c_str(), (!s.empty() && s.back() == '\n') ? "" : "\n");
                    fflush(stdout);
                }
            }
            break;
        case EXCEPTION_DEBUG_EVENT:
        {
            const auto& rec = ev.u.Exception.ExceptionRecord;
            cont = DBG_EXCEPTION_NOT_HANDLED;
            // Only report fatal exceptions that the app did not handle itself.
            if (!ev.u.Exception.dwFirstChance && isFatal(rec.ExceptionCode))
            {
                printf("\n[crashdbg] %s (0x%08lX) at 0x%p, thread %lu\n", exceptionName(rec.ExceptionCode),
                       rec.ExceptionCode, rec.ExceptionAddress, ev.dwThreadId);
                if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2)
                    printf("[crashdbg] %s address 0x%llx\n",
                           rec.ExceptionInformation[0] == 0 ? "read of" : rec.ExceptionInformation[0] == 1 ? "write to" : "execute of",
                           (unsigned long long)rec.ExceptionInformation[1]);
                printStack(ev.dwThreadId);
                fflush(stdout);
                TerminateProcess(g_process, 0xDEAD);
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            exitCode = ev.u.ExitProcess.dwExitCode;
            running = false;
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }

    printf("[crashdbg] process exited with code 0x%08lX\n", exitCode);
    return (int)exitCode;
}
