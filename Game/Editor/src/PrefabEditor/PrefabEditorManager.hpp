#pragma once

#include "PrefabEditorInstance.hpp"
#include <string>
#include <vector>
#include <memory>

class IRenderAPI;
class ReflectionRegistry;

class PrefabEditorManager
{
public:
    void initialize(IRenderAPI* render_api, ReflectionRegistry* reflection);

    // Open a prefab for editing (or focus existing if already open)
    void openPrefab(const std::string& prefab_path);

    // Render 3D previews for all open editors (call before ImGui phase)
    void renderAllPreviews();

    // Draw all open prefab editor ImGui windows
    void drawAll();

    // Cleanup all viewport resources
    void shutdown();

    bool hasOpenEditors() const { return !m_instances.empty(); }

private:
    IRenderAPI* m_render_api = nullptr;
    ReflectionRegistry* m_reflection = nullptr;
    std::vector<std::unique_ptr<PrefabEditorInstance>> m_instances;
    int m_next_id = 0;

    // Per-instance operations
    void loadPrefabIntoInstance(PrefabEditorInstance& inst);
    void savePrefabFromInstance(PrefabEditorInstance& inst);

    // Per-instance UI drawing
    void drawEditorWindow(PrefabEditorInstance& inst);
    void drawToolbar(PrefabEditorInstance& inst);
    void drawComponentsPanel(PrefabEditorInstance& inst);
    void drawDetailsPanel(PrefabEditorInstance& inst);
    void drawViewport(PrefabEditorInstance& inst);
    void drawSavePrompt(PrefabEditorInstance& inst);
};
