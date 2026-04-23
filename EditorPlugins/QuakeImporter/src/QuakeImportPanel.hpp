#pragma once
#include "Plugin/IEditorPanel.h"
#include "QuakePak.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>

struct EditorServices;

namespace QuakeImporter {

// UI panel for the Quake asset import workflow. Flow:
//   1. User picks a .pak file
//   2. Panel lists archive contents grouped by kind
//   3. User checks which entries to extract
//   4. "Extract Selected" writes raw bytes to
//      <assets>/imported/quake/<original_path>
//   5. After extraction the asset scanner picks up the new files
//
// Conversion to engine-native .cmesh / .ctex formats is OUT OF SCOPE for
// this reference plugin — see the README for how to add format conversion
// by subclassing IAssetLoader.
class QuakeImportPanel : public IEditorPanel
{
public:
    const char* getId()              const override { return "quake_import_panel"; }
    const char* getDisplayName()     const override { return "Quake Importer"; }
    const char* getDefaultDockSlot() const override { return "Center"; }
    bool        isVisibleByDefault() const override { return false; }

    void onAttach(EditorServices* services) override;
    void draw(bool* p_open) override;

    // Called from the Tools menu entry.
    void openDialog() { m_request_open = true; }

private:
    void runExtract();

    EditorServices* m_services = nullptr;
    PakArchive      m_archive;
    std::string     m_archive_path;
    std::string     m_path_input;  // text input buffer

    bool m_request_open = false;

    std::unordered_set<std::string> m_selected;   // entry names marked for extract
    std::string m_filter;

    // Async extract state.
    std::atomic<bool>   m_extracting{false};
    std::atomic<size_t> m_extract_done{0};
    size_t              m_extract_total = 0;
    std::mutex          m_extract_log_mutex;
    std::string         m_extract_log;
};

} // namespace QuakeImporter
