#pragma once

#include "EngineExport.h"
#include <string>

namespace FileDialog
{

// Open a file dialog to select a single file.
// filter: e.g. "Garden Project (*.garden)\0*.garden\0All Files (*.*)\0*.*\0"
// Returns empty string if cancelled.
ENGINE_API std::string openFile(const char* title, const char* filter = nullptr);

// Open a save-file dialog.
// Returns empty string if cancelled.
ENGINE_API std::string saveFile(const char* title, const char* filter = nullptr);

// Open a folder picker dialog.
// Returns empty string if cancelled.
ENGINE_API std::string openFolder(const char* title);

// Open a folder in the OS file explorer.
ENGINE_API void openFolderInExplorer(const std::string& path);

} // namespace FileDialog
