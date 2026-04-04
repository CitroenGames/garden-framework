#pragma once

#include <string>

namespace FileDialog
{

// Open a file dialog to select a single file.
// filter: e.g. "Garden Project (*.garden)\0*.garden\0All Files (*.*)\0*.*\0"
// Returns empty string if cancelled.
std::string openFile(const char* title, const char* filter = nullptr);

// Open a save-file dialog.
// Returns empty string if cancelled.
std::string saveFile(const char* title, const char* filter = nullptr);

// Open a folder picker dialog.
// Returns empty string if cancelled.
std::string openFolder(const char* title);

} // namespace FileDialog
