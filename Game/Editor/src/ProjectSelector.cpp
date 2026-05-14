#include "ProjectSelector.hpp"
#include "Project/ProjectManager.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/FileDialog.hpp"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi_Platform_SDL.h>

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr int kTemplateRowHeightDp = 52;

class ProjectSelectorRenderInterface final : public Rml::RenderInterface
{
public:
    explicit ProjectSelectorRenderInterface(SDL_Renderer* renderer)
        : m_renderer(renderer)
    {
        m_blendMode = SDL_ComposeCustomBlendMode(
            SDL_BLENDFACTOR_ONE,
            SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            SDL_BLENDOPERATION_ADD,
            SDL_BLENDFACTOR_ONE,
            SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            SDL_BLENDOPERATION_ADD);
    }

    void beginFrame()
    {
        SDL_SetRenderViewport(m_renderer, nullptr);
        SDL_SetRenderClipRect(m_renderer, nullptr);
        SDL_SetRenderDrawColor(m_renderer, 23, 26, 29, 255);
        SDL_RenderClear(m_renderer);
        SDL_SetRenderDrawBlendMode(m_renderer, m_blendMode);
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override
    {
        return reinterpret_cast<Rml::CompiledGeometryHandle>(new GeometryView{vertices, indices});
    }

    void RenderGeometry(Rml::CompiledGeometryHandle geometry,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override
    {
        const auto* view = reinterpret_cast<const GeometryView*>(geometry);
        const Rml::Vertex* vertices = view->vertices.data();
        const int* indices = view->indices.data();

        const int vertex_count = static_cast<int>(view->vertices.size());
        const int index_count = static_cast<int>(view->indices.size());
        std::unique_ptr<SDL_Vertex[]> sdl_vertices(new SDL_Vertex[vertex_count]);

        for (int i = 0; i < vertex_count; ++i)
        {
            sdl_vertices[i].position = {
                vertices[i].position.x + translation.x,
                vertices[i].position.y + translation.y};
            sdl_vertices[i].tex_coord = {
                vertices[i].tex_coord.x,
                vertices[i].tex_coord.y};

            const auto& color = vertices[i].colour;
            sdl_vertices[i].color = {
                color.red / 255.0f,
                color.green / 255.0f,
                color.blue / 255.0f,
                color.alpha / 255.0f};
        }

        SDL_RenderGeometry(m_renderer,
                           reinterpret_cast<SDL_Texture*>(texture),
                           sdl_vertices.get(),
                           vertex_count,
                           indices,
                           index_count);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override
    {
        delete reinterpret_cast<GeometryView*>(geometry);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                   const Rml::String& source) override
    {
        (void)texture_dimensions;
        (void)source;
        return {};
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i source_dimensions) override
    {
        RMLUI_ASSERT(source.data() && source.size() == size_t(source_dimensions.x * source_dimensions.y * 4));

        SDL_Surface* surface = SDL_CreateSurfaceFrom(source_dimensions.x,
                                                     source_dimensions.y,
                                                     SDL_PIXELFORMAT_RGBA32,
                                                     const_cast<Rml::byte*>(source.data()),
                                                     source_dimensions.x * 4);
        if (!surface)
            return {};

        SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
        SDL_DestroySurface(surface);

        if (texture)
            SDL_SetTextureBlendMode(texture, m_blendMode);

        return reinterpret_cast<Rml::TextureHandle>(texture);
    }

    void ReleaseTexture(Rml::TextureHandle texture) override
    {
        SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(texture));
    }

    void EnableScissorRegion(bool enable) override
    {
        SDL_SetRenderClipRect(m_renderer, enable ? &m_scissor : nullptr);
        m_scissorEnabled = enable;
    }

    void SetScissorRegion(Rml::Rectanglei region) override
    {
        m_scissor.x = region.Left();
        m_scissor.y = region.Top();
        m_scissor.w = region.Width();
        m_scissor.h = region.Height();

        if (m_scissorEnabled)
            SDL_SetRenderClipRect(m_renderer, &m_scissor);
    }

private:
    struct GeometryView
    {
        Rml::Span<const Rml::Vertex> vertices;
        Rml::Span<const int> indices;
    };

    SDL_Renderer* m_renderer = nullptr;
    SDL_BlendMode m_blendMode = SDL_BLENDMODE_BLEND;
    SDL_Rect m_scissor = {};
    bool m_scissorEnabled = false;
};

std::string escapeRmlText(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value)
    {
        switch (c)
        {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

void setElementText(Rml::ElementDocument* document, const char* id, const std::string& value)
{
    if (Rml::Element* element = document ? document->GetElementById(id) : nullptr)
        element->SetInnerRML(escapeRmlText(value));
}

Rml::ElementFormControlInput* getInput(Rml::ElementDocument* document, const char* id)
{
    Rml::Element* element = document ? document->GetElementById(id) : nullptr;
    return rmlui_dynamic_cast<Rml::ElementFormControlInput*>(element);
}

std::string getInputValue(Rml::ElementDocument* document, const char* id)
{
    if (Rml::ElementFormControlInput* input = getInput(document, id))
        return input->GetValue();
    return {};
}

void setInputValue(Rml::ElementDocument* document, const char* id, const std::string& value)
{
    if (Rml::ElementFormControlInput* input = getInput(document, id))
        input->SetValue(value);
}

bool parseIndexedId(const std::string& id, const char* prefix, int& index)
{
    const std::string prefix_string(prefix);
    if (id.rfind(prefix_string, 0) != 0)
        return false;

    const char* begin = id.data() + prefix_string.size();
    const char* end = id.data() + id.size();
    if (begin == end)
        return false;

    int value = -1;
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end)
        return false;

    index = value;
    return true;
}

fs::path currentPathNoThrow()
{
    std::error_code error;
    fs::path path = fs::current_path(error);
    if (error)
        return {};
    return path;
}

fs::path findProjectSelectorDocument()
{
    const fs::path exe_dir = EnginePaths::getExecutableDir();
    const fs::path cwd = currentPathNoThrow();
    const std::vector<fs::path> candidates = {
        exe_dir / ".." / "assets" / "Editor" / "RML" / "ProjectSelector.rml",
        cwd / "assets" / "Editor" / "RML" / "ProjectSelector.rml",
        cwd / ".." / "assets" / "Editor" / "RML" / "ProjectSelector.rml",
    };

    for (const fs::path& path : candidates)
    {
        std::error_code error;
        if (fs::exists(path, error) && !error)
            return path;
    }

    return {};
}

bool loadFonts()
{
    const fs::path exe_dir = EnginePaths::getExecutableDir();
    const fs::path cwd = currentPathNoThrow();
    const std::vector<fs::path> candidates = {
        exe_dir / ".." / "assets" / "fonts",
        cwd / "assets" / "fonts",
        cwd / ".." / "assets" / "fonts",
    };

    for (const fs::path& dir : candidates)
    {
        const fs::path regular = dir / "LatoLatin-Regular.ttf";
        const fs::path bold = dir / "LatoLatin-Bold.ttf";

        std::error_code error;
        if (!fs::exists(regular, error) || error)
            continue;

        bool loaded_regular = Rml::LoadFontFace(regular.generic_string(), true);
        error.clear();
        if (fs::exists(bold, error) && !error)
            Rml::LoadFontFace(bold.generic_string(), true);

        if (loaded_regular)
            return true;
    }

    fprintf(stderr, "[ProjectSelector] Warning: failed to locate RmlUi fonts.\n");
    return false;
}

void syncContextDimensions(Rml::Context* context, SDL_Renderer* renderer)
{
    int width = 0;
    int height = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &width, &height);
    if (width > 0 && height > 0)
        context->SetDimensions(Rml::Vector2i(width, height));
}

void updateDensityIndependentRatio(Rml::Context* context, SDL_Window* window)
{
    const float display_scale = SDL_GetWindowDisplayScale(window);
    if (display_scale > 0.0f)
        context->SetDensityIndependentPixelRatio(display_scale);
}

Rml::Vector2f toRenderCoordinates(SDL_Renderer* renderer, float window_x, float window_y)
{
    float render_x = window_x;
    float render_y = window_y;
    SDL_RenderCoordinatesFromWindow(renderer, window_x, window_y, &render_x, &render_y);
    return {render_x, render_y};
}

void processMouseMove(Rml::Context* context, SDL_Renderer* renderer, float x, float y)
{
    const Rml::Vector2f render_position = toRenderCoordinates(renderer, x, y);
    context->ProcessMouseMove(
        static_cast<int>(render_position.x),
        static_cast<int>(render_position.y),
        RmlSDL::GetKeyModifierState());
}

void processSelectorEvent(Rml::Context* context, SDL_Window* window, SDL_Renderer* renderer, SDL_Event& event)
{
    switch (event.type)
    {
        case SDL_EVENT_MOUSE_MOTION:
            processMouseMove(context, renderer, event.motion.x, event.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            processMouseMove(context, renderer, event.button.x, event.button.y);
            context->ProcessMouseButtonDown(
                RmlSDL::ConvertMouseButton(event.button.button),
                RmlSDL::GetKeyModifierState());
            SDL_CaptureMouse(true);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            processMouseMove(context, renderer, event.button.x, event.button.y);
            SDL_CaptureMouse(false);
            context->ProcessMouseButtonUp(
                RmlSDL::ConvertMouseButton(event.button.button),
                RmlSDL::GetKeyModifierState());
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            processMouseMove(context, renderer, event.wheel.mouse_x, event.wheel.mouse_y);
            context->ProcessMouseWheel(-event.wheel.y, RmlSDL::GetKeyModifierState());
            break;
        case SDL_EVENT_KEY_DOWN:
            context->ProcessKeyDown(
                RmlSDL::ConvertKey(event.key.key),
                RmlSDL::GetKeyModifierState());
            if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
                context->ProcessTextInput('\n');
            break;
        case SDL_EVENT_KEY_UP:
            context->ProcessKeyUp(
                RmlSDL::ConvertKey(event.key.key),
                RmlSDL::GetKeyModifierState());
            break;
        case SDL_EVENT_TEXT_INPUT:
            context->ProcessTextInput(Rml::String(&event.text.text[0]));
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (event.window.windowID == SDL_GetWindowID(window))
                syncContextDimensions(context, renderer);
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            if (event.window.windowID == SDL_GetWindowID(window))
            {
                updateDensityIndependentRatio(context, window);
                syncContextDimensions(context, renderer);
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (event.window.windowID == SDL_GetWindowID(window))
                context->ProcessMouseLeave();
            break;
        default:
            break;
    }
}

std::string buildTemplateRows(const std::vector<TemplateInfo>& templates, int selected_template)
{
    if (templates.empty())
        return "<div class=\"empty-text\">No templates were found in the Templates directory.</div>";

    std::ostringstream rows;
    rows << "<div id=\"template-spacer\" style=\"height: "
         << templates.size() * kTemplateRowHeightDp
         << "dp;\"></div>";
    for (int i = 0; i < static_cast<int>(templates.size()); ++i)
    {
        rows << "<div id=\"template-" << i << "\" class=\"template-row"
             << (i == selected_template ? " selected" : "")
             << "\" style=\"top: " << i * kTemplateRowHeightDp << "dp;\">"
             << escapeRmlText(templates[i].name)
             << "</div>";
    }
    return rows.str();
}

class ProjectSelectorListener final : public Rml::EventListener
{
public:
    ProjectSelectorListener(Rml::ElementDocument* document,
                            const std::vector<TemplateInfo>& templates,
                            ProjectManager& project_manager,
                            std::string& result_path,
                            bool& done)
        : m_document(document)
        , m_templates(templates)
        , m_projectManager(project_manager)
        , m_resultPath(result_path)
        , m_done(done)
    {
        if (!m_templates.empty())
            m_selectedTemplate = 0;
    }

    void attach()
    {
        if (!m_document)
            return;

        m_document->AddEventListener(Rml::EventId::Keydown, this, true);
        addClickListener("project-browser-tab");
        addClickListener("open-existing-tab");
        addClickListener("browse-dir-button");
        addClickListener("browse-open-button");
        addClickListener("create-project-button");
        addClickListener("open-project-button");

        refreshTemplateList();
        setActiveTab(Tab::ProjectBrowser);
        syncUi();
    }

    void detach()
    {
        if (!m_document)
            return;

        m_document->RemoveEventListener(Rml::EventId::Keydown, this, true);
        removeClickListener("project-browser-tab");
        removeClickListener("open-existing-tab");
        removeClickListener("browse-dir-button");
        removeClickListener("browse-open-button");
        removeClickListener("create-project-button");
        removeClickListener("open-project-button");
        detachTemplateListeners();
    }

    void syncUi()
    {
        updateCreateState();
        updateOpenState();
    }

    void handleTemplatePointerPress(Rml::Vector2f point)
    {
        Rml::Element* list = m_document ? m_document->GetElementById("template-list") : nullptr;
        Rml::Element* first_row = m_document ? m_document->GetElementById("template-0") : nullptr;
        if (!list || !first_row || !list->IsPointWithinElement(point))
            return;

        const float row_height = first_row->GetOffsetHeight();
        if (row_height <= 0.0f)
            return;

        const float content_top = list->GetAbsoluteOffset(Rml::BoxArea::Padding).y;
        const float row_y = point.y - content_top + list->GetScrollTop();
        const int index = static_cast<int>(row_y / row_height);
        if (index >= 0 && index < static_cast<int>(m_templates.size()))
            selectTemplate(index);
    }

    void ProcessEvent(Rml::Event& event) override
    {
        if (event == Rml::EventId::Keydown)
        {
            const auto key = static_cast<Rml::Input::KeyIdentifier>(
                event.GetParameter<int>("key_identifier", 0));
            if (key == Rml::Input::KI_ESCAPE)
            {
                m_done = true;
                event.StopPropagation();
            }
            return;
        }

        Rml::Element* element = resolveActionElement(event.GetTargetElement());
        if (!element)
            element = resolveActionElement(event.GetCurrentElement());
        if (!element)
            return;

        processAction(element->GetId());
        event.StopPropagation();
    }

private:
    enum class Tab
    {
        ProjectBrowser,
        OpenExisting
    };

    void addClickListener(const char* id)
    {
        if (Rml::Element* element = m_document->GetElementById(id))
            element->AddEventListener(Rml::EventId::Click, this);
    }

    void removeClickListener(const char* id)
    {
        if (Rml::Element* element = m_document->GetElementById(id))
            element->RemoveEventListener(Rml::EventId::Click, this);
    }

    void attachTemplateListeners()
    {
        for (int i = 0; i < static_cast<int>(m_templates.size()); ++i)
        {
            if (Rml::Element* row = m_document->GetElementById("template-" + std::to_string(i)))
                row->AddEventListener(Rml::EventId::Click, this);
        }
    }

    void detachTemplateListeners()
    {
        for (int i = 0; i < static_cast<int>(m_templates.size()); ++i)
        {
            if (Rml::Element* row = m_document->GetElementById("template-" + std::to_string(i)))
                row->RemoveEventListener(Rml::EventId::Click, this);
        }
    }

    Rml::Element* resolveActionElement(Rml::Element* element) const
    {
        while (element)
        {
            const std::string id = element->GetId();
            int template_index = -1;
            if (id == "project-browser-tab" ||
                id == "open-existing-tab" ||
                id == "browse-dir-button" ||
                id == "browse-open-button" ||
                id == "create-project-button" ||
                id == "open-project-button" ||
                parseIndexedId(id, "template-", template_index))
            {
                return element;
            }
            element = element->GetParentNode();
        }

        return nullptr;
    }

    void processAction(const std::string& id)
    {
        int template_index = -1;
        if (parseIndexedId(id, "template-", template_index))
        {
            selectTemplate(template_index);
            return;
        }

        if (id == "project-browser-tab")
            setActiveTab(Tab::ProjectBrowser);
        else if (id == "open-existing-tab")
            setActiveTab(Tab::OpenExisting);
        else if (id == "browse-dir-button")
            browseProjectDirectory();
        else if (id == "browse-open-button")
            browseProjectFile();
        else if (id == "create-project-button")
            createProject();
        else if (id == "open-project-button")
            openProject();
    }

    void setActiveTab(Tab tab)
    {
        m_activeTab = tab;
        setClass("project-browser-tab", "active", m_activeTab == Tab::ProjectBrowser);
        setClass("create-panel", "active", m_activeTab == Tab::ProjectBrowser);
        setClass("open-existing-tab", "active", m_activeTab == Tab::OpenExisting);
        setClass("open-panel", "active", m_activeTab == Tab::OpenExisting);
        clearStatus();
        syncUi();
    }

    void refreshTemplateList()
    {
        if (Rml::Element* list = m_document->GetElementById("template-list"))
            list->SetInnerRML(buildTemplateRows(m_templates, m_selectedTemplate));

        attachTemplateListeners();
        if (m_templates.empty())
            m_selectedTemplate = -1;
        selectTemplate(m_selectedTemplate);
    }

    void selectTemplate(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_templates.size()))
        {
            m_selectedTemplate = -1;
            setElementText(m_document, "selected-template-name", "None");
            setElementText(m_document, "selected-template-path", "No template selected.");
            updateCreateState();
            return;
        }

        m_selectedTemplate = index;
        for (int i = 0; i < static_cast<int>(m_templates.size()); ++i)
            setClass("template-" + std::to_string(i), "selected", i == m_selectedTemplate);

        setElementText(m_document, "selected-template-name", m_templates[m_selectedTemplate].name);
        setElementText(m_document, "selected-template-path", m_templates[m_selectedTemplate].path);
        updateCreateState();
    }

    void browseProjectDirectory()
    {
        std::string folder = FileDialog::openFolder("Select Project Directory");
        if (!folder.empty())
        {
            setInputValue(m_document, "new-project-dir", folder);
            clearStatus();
            syncUi();
        }
    }

    void browseProjectFile()
    {
        std::string file = FileDialog::openFile(
            "Open Garden Project",
            "Garden Project (*.garden)\0*.garden\0All Files (*.*)\0*.*\0");
        if (!file.empty())
        {
            setInputValue(m_document, "open-project-path", file);
            clearStatus();
            syncUi();
        }
    }

    bool canCreate() const
    {
        return m_selectedTemplate >= 0 &&
               m_selectedTemplate < static_cast<int>(m_templates.size()) &&
               !getInputValue(m_document, "new-project-name").empty() &&
               !getInputValue(m_document, "new-project-dir").empty();
    }

    void createProject()
    {
        if (!canCreate())
        {
            setStatus("Name, directory, and template are required.", true);
            return;
        }

        const std::string name = getInputValue(m_document, "new-project-name");
        const std::string directory = getInputValue(m_document, "new-project-dir");

        bool success = m_projectManager.createProjectFromTemplate(
            m_templates[m_selectedTemplate].path,
            directory,
            name);
        if (success)
        {
            m_resultPath = m_projectManager.getProjectFilePath();
            m_done = true;
        }
        else
        {
            setStatus("Failed to create the project. Check the directory and template files.", true);
            fprintf(stderr, "[ProjectSelector] Failed to create project '%s' in '%s'\n",
                    name.c_str(), directory.c_str());
        }
    }

    void openProject()
    {
        const std::string path_text = getInputValue(m_document, "open-project-path");
        if (path_text.empty())
        {
            setStatus("Choose a .garden file first.", true);
            return;
        }

        const fs::path path(path_text);
        std::error_code exists_error;
        bool project_exists = fs::exists(path, exists_error);
        if (exists_error || !project_exists)
        {
            setStatus("That project file does not exist.", true);
            return;
        }

        std::error_code absolute_error;
        fs::path absolute_path = fs::absolute(path, absolute_error);
        if (absolute_error)
        {
            setStatus("Could not resolve that project path.", true);
            return;
        }

        m_resultPath = absolute_path.string();
        m_done = true;
    }

    void updateCreateState()
    {
        const bool enabled = canCreate();
        setClass("create-project-button", "disabled", !enabled);
        setElementText(m_document,
                       "create-hint",
                       enabled ? "Ready to create." : "Name, directory, and template are required.");
    }

    void updateOpenState()
    {
        const std::string path = getInputValue(m_document, "open-project-path");
        const bool enabled = !path.empty();
        setClass("open-project-button", "disabled", !enabled);
        setElementText(m_document,
                       "open-hint",
                       enabled ? "Ready to open." : "Choose a .garden file first.");
        setElementText(m_document,
                       "open-current-path",
                       enabled ? path : "No project selected.");
    }

    void setStatus(const std::string& message, bool error)
    {
        if (Rml::Element* status = m_document->GetElementById("status"))
        {
            status->SetInnerRML(escapeRmlText(message));
            status->SetClass("visible", !message.empty());
            status->SetClass("error", error);
        }
    }

    void clearStatus()
    {
        setStatus({}, false);
    }

    void setClass(const char* id, const char* class_name, bool enabled)
    {
        if (Rml::Element* element = m_document->GetElementById(id))
            element->SetClass(class_name, enabled);
    }

    void setClass(const std::string& id, const char* class_name, bool enabled)
    {
        if (Rml::Element* element = m_document->GetElementById(id))
            element->SetClass(class_name, enabled);
    }

    Rml::ElementDocument* m_document = nullptr;
    const std::vector<TemplateInfo>& m_templates;
    ProjectManager& m_projectManager;
    std::string& m_resultPath;
    bool& m_done;
    Tab m_activeTab = Tab::ProjectBrowser;
    int m_selectedTemplate = -1;
};
} // namespace

std::string ProjectSelector::run()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        fprintf(stderr, "[ProjectSelector] SDL_Init failed: %s\n", SDL_GetError());
        return "";
    }

    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    SDL_Window* window = SDL_CreateWindow(
        "Garden - Project Browser",
        1040,
        700,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    if (!window)
    {
        fprintf(stderr, "[ProjectSelector] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return "";
    }

    SDL_SetWindowMinimumSize(window, 1040, 700);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
    {
        fprintf(stderr, "[ProjectSelector] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }
    SDL_SetRenderVSync(renderer, 1);

    SystemInterface_SDL system_interface;
    system_interface.SetWindow(window);
    ProjectSelectorRenderInterface render_interface(renderer);

    Rml::SetSystemInterface(&system_interface);
    Rml::SetRenderInterface(&render_interface);

    if (!Rml::Initialise())
    {
        fprintf(stderr, "[ProjectSelector] RmlUi initialise failed.\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }

    int width = 900;
    int height = 600;
    SDL_GetCurrentRenderOutputSize(renderer, &width, &height);

    Rml::Context* context = Rml::CreateContext("garden_editor_project_selector", Rml::Vector2i(width, height));
    if (!context)
    {
        fprintf(stderr, "[ProjectSelector] RmlUi context creation failed.\n");
        Rml::Shutdown();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }

    updateDensityIndependentRatio(context, window);
    loadFonts();

    const fs::path document_path = findProjectSelectorDocument();
    if (document_path.empty())
    {
        fprintf(stderr, "[ProjectSelector] Failed to find assets/Editor/RML/ProjectSelector.rml.\n");
        const Rml::String context_name = context->GetName();
        Rml::RemoveContext(context_name);
        Rml::Shutdown();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }

    Rml::ElementDocument* document = context->LoadDocument(document_path.generic_string());
    if (!document)
    {
        fprintf(stderr, "[ProjectSelector] Failed to load '%s'.\n", document_path.string().c_str());
        const Rml::String context_name = context->GetName();
        Rml::RemoveContext(context_name);
        Rml::Shutdown();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }

    const fs::path exe_dir = EnginePaths::getExecutableDir();
    const fs::path templates_dir = exe_dir / ".." / "Templates";
    std::vector<TemplateInfo> available_templates =
        ProjectManager::discoverTemplates(templates_dir.string());

    ProjectManager project_manager;
    std::string result_path;
    bool done = false;

    ProjectSelectorListener listener(document, available_templates, project_manager, result_path, done);
    listener.attach();
    document->Show();
    context->Update();

    while (!done)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
                done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
                done = true;

            processSelectorEvent(context, window, renderer, event);
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.windowID == SDL_GetWindowID(window) &&
                event.button.button == SDL_BUTTON_LEFT)
            {
                const Rml::Vector2f render_position = toRenderCoordinates(renderer, event.button.x, event.button.y);
                listener.handleTemplatePointerPress(render_position);
            }
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        listener.syncUi();
        context->Update();
        render_interface.beginFrame();
        context->Render();
        SDL_RenderPresent(renderer);
    }

    listener.detach();
    document->Close();
    context->Update();
    const Rml::String context_name = context->GetName();
    Rml::RemoveContext(context_name);
    Rml::Shutdown();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return result_path;
}
