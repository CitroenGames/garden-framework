#pragma once

#include "EngineGraphicsExport.h"
#include "Graphics/RenderAPI.hpp"
#include <SDL3/SDL.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Forward declarations
namespace Rml { class Context; class RenderInterface; }
class SystemInterface_SDL;

class ENGINE_GRAPHICS_API RmlUiManager
{
public:
    static RmlUiManager& get();

    // Initialization - call after render API is initialized
    bool initialize(SDL_Window* window, IRenderAPI* renderAPI, RenderAPIType apiType);
    void shutdown();

    // Per-frame calls
    void beginFrame();
    void beginFrame(int width, int height);
    void render();
    void beginEditorFrame(int width, int height);
    void renderEditor();

    // Event handling - returns true if RmlUi consumed the event
    bool processEvent(SDL_Event& event);
    bool processEditorEvent(SDL_Event& event);

    // State queries
    bool isInitialized() const;

    // Context access
    Rml::Context* getContext() const;
    Rml::Context* getEditorContext() const;

    // Document management
    void* loadDocument(const char* path);
    void* loadEditorDocument(const char* path);
    void closeDocument(void* document);
    void closeEditorDocument(void* document);
    void toggleDebugger();

    enum class EditorStyleProperty
    {
        Width,
        Height,
        Left,
        Right,
        Top,
        Bottom,
    };

    enum class EditorEventType
    {
        Click,
        Mouseover,
        Mousedown,
        Change,
    };

    struct EditorElementEvent
    {
        const char* element_id = nullptr;
        const char* data_command = nullptr;
        const char* data_tab = nullptr;
        const char* data_map = nullptr;
        const char* data_setting = nullptr;
        const char* value = nullptr;
        float mouse_x = 0.0f;
        float mouse_y = 0.0f;
    };

    using EditorElementEventFn = void (*)(const EditorElementEvent* event, void* user_data);
    using EditorEventHandle = std::uint64_t;

    void setEditorElementText(void* document, const char* id, const char* text);
    void setEditorElementClass(void* document, const char* id, const char* class_name, bool enabled);
    void setEditorElementAttribute(void* document, const char* id, const char* attribute, const char* value);
    void setEditorElementProperty(void* document, const char* id, const char* property, const char* value);
    void setEditorElementStyleDp(void* document, const char* id, EditorStyleProperty property, int value);
    void setDocumentVisible(void* document, bool visible);
    EditorEventHandle registerEditorElementEvent(
        void* document,
        const char* id,
        EditorEventType type,
        EditorElementEventFn callback,
        void* user_data);
    EditorEventHandle registerEditorClassEvent(
        void* document,
        const char* class_name,
        EditorEventType type,
        EditorElementEventFn callback,
        void* user_data);
    void unregisterEditorElementEvent(EditorEventHandle handle);

    // C-safe data model API for hot-loaded game modules. The model values are
    // owned by EngineGraphics so RmlUi never reads STL objects from game DLLs.
    void* createDataModel(const char* name);
    void removeDataModel(void* model);
    bool dataModelBindInt(void* model, const char* name, int value);
    bool dataModelBindBool(void* model, const char* name, bool value);
    bool dataModelBindString(void* model, const char* name, const char* value);
    bool dataModelSetInt(void* model, const char* name, int value);
    bool dataModelSetBool(void* model, const char* name, bool value);
    bool dataModelSetString(void* model, const char* name, const char* value);
    void dataModelDirtyAll(void* model);

private:
    RmlUiManager() = default;
    ~RmlUiManager();
    RmlUiManager(const RmlUiManager&) = delete;
    RmlUiManager& operator=(const RmlUiManager&) = delete;

    class EditorEventRegistration;

    bool initD3D12(SDL_Window* window, IRenderAPI* api);
    bool initVulkan(SDL_Window* window, IRenderAPI* api);
    bool initMetal(SDL_Window* window, IRenderAPI* api);
    void clearEditorEventRegistrationsForDocument(void* document);

    bool m_initialized = false;
    RenderAPIType m_apiType = DefaultRenderAPI;
    SDL_Window* m_window = nullptr;
    IRenderAPI* m_renderAPI = nullptr;

    Rml::Context* m_context = nullptr;
    Rml::Context* m_editorContext = nullptr;
    Rml::RenderInterface* m_renderInterface = nullptr;
    SystemInterface_SDL* m_systemInterface = nullptr;
    bool m_debuggerVisible = false;
    EditorEventHandle m_nextEditorEventHandle = 1;
    std::unordered_map<EditorEventHandle, EditorEventRegistration*> m_editorEventRegistrations;
    std::vector<void*> m_dataModels;
    std::vector<void*> m_documents;
    std::vector<void*> m_editorDocuments;
};
