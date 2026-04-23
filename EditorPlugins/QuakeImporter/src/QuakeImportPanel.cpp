#include "QuakeImportPanel.hpp"
#include "Plugin/EditorPluginAPI.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace QuakeImporter {

void QuakeImportPanel::onAttach(EditorServices* services)
{
    m_services = services;
    m_path_input.reserve(512);
}

static void logInfo(EditorServices* s, const std::string& msg)
{
    if (s && s->log_info) s->log_info(msg.c_str());
}
static void logWarn(EditorServices* s, const std::string& msg)
{
    if (s && s->log_warn) s->log_warn(msg.c_str());
}
static void logError(EditorServices* s, const std::string& msg)
{
    if (s && s->log_error) s->log_error(msg.c_str());
}

void QuakeImportPanel::draw(bool* p_open)
{
    // Menu callback sets this flag so the panel opens even if previously closed.
    if (m_request_open)
    {
        if (p_open) *p_open = true;
        m_request_open = false;
    }

    if (!ImGui::Begin("Quake Importer", p_open)) { ImGui::End(); return; }

    // ---- Archive picker ----
    ImGui::Text("PAK file:");
    ImGui::SameLine();
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", m_path_input.c_str());
    if (ImGui::InputText("##pak_path", buf, sizeof(buf)))
        m_path_input = buf;

    ImGui::SameLine();
    if (ImGui::Button("Open"))
    {
        m_archive.close();
        m_selected.clear();
        if (m_archive.open(m_path_input))
        {
            m_archive_path = m_path_input;
            logInfo(m_services, "[QuakeImporter] Opened '" + m_archive_path +
                                "' (" + std::to_string(m_archive.entries().size()) + " entries)");
        }
        else
        {
            logError(m_services, "[QuakeImporter] Failed to open '" + m_path_input + "' — not a Quake PAK?");
            m_archive_path.clear();
        }
    }

    if (!m_archive.isOpen())
    {
        ImGui::TextDisabled("Enter an absolute path to a Quake 1 pak0.pak / pak1.pak and press Open.");
        ImGui::End();
        return;
    }

    ImGui::Text("Archive: %s", m_archive_path.c_str());
    ImGui::Text("Entries: %zu", m_archive.entries().size());

    ImGui::InputText("Filter", m_filter.data(), 0);
    // InputText without a modifiable buffer is awkward; use the simple form:
    {
        static char fbuf[128];
        std::snprintf(fbuf, sizeof(fbuf), "%s", m_filter.c_str());
        if (ImGui::InputText("##filter", fbuf, sizeof(fbuf))) m_filter = fbuf;
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Select All"))
    {
        for (const auto& e : m_archive.entries()) m_selected.insert(e.name);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) m_selected.clear();

    ImGui::Separator();

    // ---- Tree view grouped by kind ----
    std::map<EntryKind, std::vector<const PakEntry*>> grouped;
    for (const auto& e : m_archive.entries())
        grouped[classify(e.name)].push_back(&e);

    ImGui::BeginChild("##entries", ImVec2(0, -80), true);
    for (auto& [kind, entries] : grouped)
    {
        ImGui::PushID((int)kind);
        if (ImGui::TreeNodeEx(kindName(kind), ImGuiTreeNodeFlags_DefaultOpen,
                              "%s (%zu)", kindName(kind), entries.size()))
        {
            for (const auto* e : entries)
            {
                if (!m_filter.empty() && e->name.find(m_filter) == std::string::npos) continue;
                bool checked = m_selected.count(e->name) > 0;
                if (ImGui::Checkbox(e->name.c_str(), &checked))
                {
                    if (checked) m_selected.insert(e->name);
                    else         m_selected.erase(e->name);
                }
                ImGui::SameLine(ImGui::GetWindowWidth() - 100);
                ImGui::TextDisabled("%u B", e->size);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- Action bar ----
    ImGui::Separator();
    bool can_extract = !m_selected.empty() && !m_extracting;
    if (!can_extract) ImGui::BeginDisabled();
    if (ImGui::Button("Extract Selected"))
        runExtract();
    if (!can_extract) ImGui::EndDisabled();

    ImGui::SameLine();
    if (m_extracting)
    {
        float pct = m_extract_total ? (float)m_extract_done.load() / (float)m_extract_total : 0.0f;
        ImGui::ProgressBar(pct, ImVec2(-1, 0),
            (std::to_string(m_extract_done.load()) + " / " + std::to_string(m_extract_total)).c_str());
    }
    else
    {
        ImGui::TextDisabled("%zu selected", m_selected.size());
    }

    {
        std::lock_guard<std::mutex> lock(m_extract_log_mutex);
        if (!m_extract_log.empty())
        {
            ImGui::Separator();
            ImGui::TextUnformatted("Last extract:");
            ImGui::BeginChild("##log", ImVec2(0, 80), true);
            ImGui::TextUnformatted(m_extract_log.c_str());
            ImGui::EndChild();
        }
    }

    ImGui::End();
}

// Extraction is run on a background job via EditorServices::run_background.
// The panel's archive stays open for the duration — users should not close
// the panel mid-extract. (A robust implementation would refcount the archive;
// out of scope for the reference plugin.)
void QuakeImportPanel::runExtract()
{
    if (m_extracting) return;
    if (!m_services || !m_services->project.assets_root)
    {
        logError(m_services, "[QuakeImporter] No project loaded — cannot extract");
        return;
    }

    m_extracting      = true;
    m_extract_done    = 0;
    m_extract_total   = m_selected.size();
    {
        std::lock_guard<std::mutex> lock(m_extract_log_mutex);
        m_extract_log.clear();
    }

    // Snapshot the selection + destination so the worker thread doesn't race
    // with UI state.
    struct Job {
        QuakeImportPanel* self;
        std::vector<std::string> names;
        std::string dest_root;
    };
    auto* job = new Job{this, {}, {}};
    job->names.reserve(m_selected.size());
    for (const auto& n : m_selected) job->names.push_back(n);

    job->dest_root = (fs::path(m_services->project.assets_root) / "imported" / "quake").string();

    auto worker = [](void* user) {
        std::unique_ptr<Job> j(static_cast<Job*>(user));
        auto* self = j->self;

        std::error_code ec;
        fs::create_directories(j->dest_root, ec);

        size_t failures = 0;
        for (const auto& name : j->names)
        {
            // Locate the entry in the archive.
            const PakEntry* target = nullptr;
            for (const auto& e : self->m_archive.entries())
                if (e.name == name) { target = &e; break; }

            if (!target)
            {
                failures++;
                self->m_extract_done.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            std::vector<uint8_t> bytes;
            if (!self->m_archive.readEntry(*target, bytes))
            {
                failures++;
                self->m_extract_done.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            fs::path out = fs::path(j->dest_root) / target->name;
            fs::create_directories(out.parent_path(), ec);
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            if (!f) { failures++; }
            else    { f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }

            self->m_extract_done.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(self->m_extract_log_mutex);
            self->m_extract_log = "Extracted " +
                std::to_string(j->names.size() - failures) + " / " +
                std::to_string(j->names.size()) + " files to " + j->dest_root;
        }
        self->m_extracting = false;
    };

    if (m_services->run_background)
    {
        m_services->run_background(job, worker, "QuakeImporter.Extract");
    }
    else
    {
        // Fallback: run synchronously (blocks main thread).
        worker(job);
    }
}

} // namespace QuakeImporter
