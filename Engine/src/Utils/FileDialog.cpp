#include "FileDialog.hpp"

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#   include <commdlg.h>
#   include <shlobj.h>
#   pragma comment(lib, "comdlg32.lib")
#   pragma comment(lib, "shell32.lib")
#   pragma comment(lib, "ole32.lib")
#endif

#include <cstdio>
#include <cstring>

namespace FileDialog
{

#ifdef _WIN32

std::string openFile(const char* title, const char* filter)
{
    char path[MAX_PATH] = {};

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter ? filter : "All Files (*.*)\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn))
        return std::string(path);

    return "";
}

std::string saveFile(const char* title, const char* filter)
{
    char path[MAX_PATH] = {};

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter ? filter : "All Files (*.*)\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (GetSaveFileNameA(&ofn))
        return std::string(path);

    return "";
}

std::string openFolder(const char* title)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    char path[MAX_PATH] = {};

    BROWSEINFOA bi = {};
    bi.hwndOwner = nullptr;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if (pidl)
    {
        SHGetPathFromIDListA(pidl, path);
        CoTaskMemFree(pidl);
    }

    CoUninitialize();
    return std::string(path);
}

#elif defined(__APPLE__)

// macOS: use osascript (AppleScript) for simplicity without ObjC bridging
std::string openFile(const char* title, const char* filter)
{
    (void)filter;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'set f to POSIX path of (choose file with prompt \"%s\")' 2>/dev/null",
        title ? title : "Open File");

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";

    char buf[1024] = {};
    fgets(buf, sizeof(buf), pipe);
    pclose(pipe);

    // Trim trailing newline
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    return std::string(buf);
}

std::string saveFile(const char* title, const char* filter)
{
    (void)filter;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'set f to POSIX path of (choose file name with prompt \"%s\")' 2>/dev/null",
        title ? title : "Save File");

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";

    char buf[1024] = {};
    fgets(buf, sizeof(buf), pipe);
    pclose(pipe);

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    return std::string(buf);
}

std::string openFolder(const char* title)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'set f to POSIX path of (choose folder with prompt \"%s\")' 2>/dev/null",
        title ? title : "Select Folder");

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";

    char buf[1024] = {};
    fgets(buf, sizeof(buf), pipe);
    pclose(pipe);

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    return std::string(buf);
}

#else

// Linux: use zenity or kdialog
std::string openFile(const char* title, const char* filter)
{
    (void)filter;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "zenity --file-selection --title=\"%s\" 2>/dev/null",
        title ? title : "Open File");

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";

    char buf[1024] = {};
    fgets(buf, sizeof(buf), pipe);
    pclose(pipe);

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    return std::string(buf);
}

std::string saveFile(const char* title, const char* filter)
{
    (void)filter;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "zenity --file-selection --save --confirm-overwrite --title=\"%s\" 2>/dev/null",
        title ? title : "Save File");

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";

    char buf[1024] = {};
    fgets(buf, sizeof(buf), pipe);
    pclose(pipe);

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    return std::string(buf);
}

std::string openFolder(const char* title)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "zenity --file-selection --directory --title=\"%s\" 2>/dev/null",
        title ? title : "Select Folder");

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";

    char buf[1024] = {};
    fgets(buf, sizeof(buf), pipe);
    pclose(pipe);

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    return std::string(buf);
}

#endif

} // namespace FileDialog
