#include "EnginePicker.hpp"

#define SDL_MAIN_HANDLED
#include "RmlSdlRenderer.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi_Platform_SDL.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#elif defined(__APPLE__)
#   include <mach-o/dyld.h>
#else
#   include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
constexpr int kEngineRowHeightDp = 76;

fs::path getExecutableDir()
{
#ifdef _WIN32
    char buffer[32768] = {};
    DWORD size = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (size > 0 && size < sizeof(buffer))
        return fs::path(std::string(buffer, size)).parent_path();
#elif defined(__APPLE__)
    char buffer[4096] = {};
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0)
        return fs::path(buffer).parent_path();
#else
    char buffer[4096] = {};
    ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size > 0)
    {
        buffer[size] = '\0';
        return fs::path(buffer).parent_path();
    }
#endif
    return fs::current_path();
}

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

bool loadFontFaceFromFile(const fs::path& path,
                          Rml::Style::FontWeight weight,
                          bool fallback_face)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;

    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0)
        return false;

    static std::vector<std::vector<Rml::byte>> font_data_cache;
    std::vector<Rml::byte> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size))
        return false;

    font_data_cache.push_back(std::move(data));
    const auto& cached = font_data_cache.back();
    return Rml::LoadFontFace(
        Rml::Span<const Rml::byte>(cached.data(), cached.size()),
        "LatoLatin",
        Rml::Style::FontStyle::Normal,
        weight,
        fallback_face);
}

bool loadFonts(const std::vector<EngineEntry>& engines)
{
    const fs::path exe_dir = getExecutableDir();
    const fs::path cwd = fs::current_path();
    std::vector<fs::path> candidates = {
        exe_dir / ".." / "assets" / "fonts",
        cwd / "assets" / "fonts",
        cwd / ".." / "assets" / "fonts",
    };

    for (const EngineEntry& engine : engines)
    {
        if (engine.path_exists && !engine.path.empty())
            candidates.push_back(fs::path(engine.path) / "assets" / "fonts");
    }

    for (const fs::path& dir : candidates)
    {
        const fs::path regular = dir / "LatoLatin-Regular.ttf";
        const fs::path bold = dir / "LatoLatin-Bold.ttf";
        if (!fs::exists(regular))
            continue;

        bool loaded_regular = loadFontFaceFromFile(regular, Rml::Style::FontWeight::Normal, true);
        bool loaded_bold = fs::exists(bold)
            ? loadFontFaceFromFile(bold, Rml::Style::FontWeight::Bold, true)
            : false;
        if (loaded_regular)
        {
            if (!loaded_bold)
                fprintf(stderr, "Warning: failed to load bold RmlUi font '%s'\n", bold.string().c_str());
            return true;
        }
    }

    fprintf(stderr, "Warning: failed to locate RmlUi fonts for GardenCLI.\n");
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

void processPickerMouseMove(Rml::Context* context, SDL_Renderer* renderer, float x, float y)
{
    const Rml::Vector2f render_position = toRenderCoordinates(renderer, x, y);
    context->ProcessMouseMove(
        static_cast<int>(render_position.x),
        static_cast<int>(render_position.y),
        RmlSDL::GetKeyModifierState());
}

void processPickerEvent(Rml::Context* context, SDL_Window* window, SDL_Renderer* renderer, SDL_Event& event)
{
    switch (event.type)
    {
        case SDL_EVENT_MOUSE_MOTION:
            processPickerMouseMove(context, renderer, event.motion.x, event.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            processPickerMouseMove(context, renderer, event.button.x, event.button.y);
            context->ProcessMouseButtonDown(
                RmlSDL::ConvertMouseButton(event.button.button),
                RmlSDL::GetKeyModifierState());
            SDL_CaptureMouse(true);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            processPickerMouseMove(context, renderer, event.button.x, event.button.y);
            SDL_CaptureMouse(false);
            context->ProcessMouseButtonUp(
                RmlSDL::ConvertMouseButton(event.button.button),
                RmlSDL::GetKeyModifierState());
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            processPickerMouseMove(context, renderer, event.wheel.mouse_x, event.wheel.mouse_y);
            context->ProcessMouseWheel(-event.wheel.y, RmlSDL::GetKeyModifierState());
            break;
        case SDL_EVENT_KEY_DOWN:
        {
            context->ProcessKeyDown(
                RmlSDL::ConvertKey(event.key.key),
                RmlSDL::GetKeyModifierState());

            if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
                context->ProcessTextInput('\n');
            break;
        }
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

std::string buildEngineRows(const std::vector<EngineEntry>& engines)
{
    if (engines.empty())
    {
        return
            "<div id=\"empty-state\">"
            "<div class=\"empty-title\">No engines are registered.</div>"
            "<div class=\"empty-copy\">Register an engine from the command line:</div>"
            "<div class=\"empty-command\">garden register-engine --path &lt;engine_dir&gt;</div>"
            "</div>";
    }

    std::ostringstream rows;
    rows << "<div id=\"engine-spacer\"></div>";
    for (size_t i = 0; i < engines.size(); ++i)
    {
        const EngineEntry& engine = engines[i];
        const bool missing = !engine.path_exists;
        rows << "<div id=\"engine-" << i << "\" class=\"engine-row"
             << (missing ? " missing" : "") << "\">";
        rows << "<div class=\"engine-title\">";
        rows << "<span class=\"engine-id\">" << escapeRmlText(engine.id) << "</span>";
        rows << "<span class=\"engine-version\">v"
             << escapeRmlText(engine.version.empty() ? "-" : engine.version) << "</span>";
        if (missing)
            rows << "<span class=\"engine-missing\">MISSING</span>";
        rows << "</div>";
        rows << "<div class=\"engine-path\">" << escapeRmlText(engine.path) << "</div>";
        rows << "</div>";
    }
    return rows.str();
}

std::string buildEngineRowCss(const std::vector<EngineEntry>& engines)
{
    if (engines.empty())
        return {};

    std::ostringstream css;
    css << "#engine-spacer {\n"
        << "    display: block;\n"
        << "    width: 1px;\n"
        << "    height: " << engines.size() * kEngineRowHeightDp << "dp;\n"
        << "}\n";

    for (size_t i = 0; i < engines.size(); ++i)
        css << "#engine-" << i << " { top: " << i * kEngineRowHeightDp << "dp; }\n";

    return css.str();
}

std::string buildDocument(const std::vector<EngineEntry>& engines, const std::string& projectName)
{
    std::ostringstream document;
    document << R"(
<rml>
<head>
<title>Garden - Select Engine</title>
<style>
body {
    display: block;
    margin: 0;
    padding: 0;
    width: 100%;
    height: 100%;
    background-color: #171a1d;
    color: #e2e5e8;
    font-family: LatoLatin;
    font-size: 14dp;
}
#root {
    display: block;
    position: absolute;
    left: 0;
    top: 0;
    right: 0;
    bottom: 0;
    padding: 28dp;
}
#header {
    display: block;
    position: absolute;
    left: 28dp;
    top: 26dp;
    right: 28dp;
    height: 66dp;
    border-bottom: 1dp #333b42;
}
#title {
    display: block;
    font-size: 24dp;
    font-weight: bold;
    color: #f5f7f8;
}
#subtitle {
    display: block;
    margin-top: 7dp;
    color: #a7b0ba;
}
#project-name {
    color: #66c2d1;
}
#engine-list {
    display: block;
    position: absolute;
    left: 28dp;
    top: 122dp;
    right: 28dp;
    bottom: 88dp;
    overflow-y: scroll;
    background-color: #11161a;
    border: 1dp #344049;
    border-radius: 4dp;
}
.engine-row {
    display: block;
    position: absolute;
    left: 0dp;
    right: 0dp;
    height: 76dp;
    width: auto;
    box-sizing: border-box;
    padding: 13dp 16dp 13dp 13dp;
    border-left: 3dp #11161a;
    border-bottom: 1dp #27323a;
    background-color: transparent;
    cursor: pointer;
    focus: auto;
    tab-index: auto;
}
.engine-row:hover {
    background-color: #1b242a;
    border-left-color: #43525a;
}
.engine-row.selected {
    background-color: #203940;
    border-left-color: #69c7d6;
    border-bottom-color: #35525b;
}
.engine-row.missing {
    color: #7d858d;
    background-color: #13181c;
    border-left-color: #3f282d;
    cursor: arrow;
    focus: none;
    tab-index: none;
}
.engine-row.missing:hover {
    background-color: #171d21;
}
.engine-title {
    display: block;
    width: 100%;
    box-sizing: border-box;
    height: 22dp;
    pointer-events: none;
}
.engine-id {
    font-size: 16dp;
    font-weight: bold;
    color: #f0f3f4;
    pointer-events: none;
}
.missing .engine-id {
    color: #858e96;
}
.engine-version {
    margin-left: 12dp;
    color: #96a7b5;
    pointer-events: none;
}
.engine-missing {
    margin-left: 12dp;
    padding: 3dp 7dp;
    background-color: #563137;
    color: #f0a3aa;
    font-size: 12dp;
    pointer-events: none;
}
.engine-path {
    display: block;
    width: 100%;
    box-sizing: border-box;
    margin-top: 7dp;
    color: #a5b0ba;
    font-size: 13dp;
    pointer-events: none;
}
.missing .engine-path {
    color: #687179;
}
)";
    document << buildEngineRowCss(engines);
    document << R"(
#empty-state {
    display: block;
    padding: 28dp;
    color: #b8c0cc;
}
.empty-title {
    display: block;
    font-size: 16dp;
    font-weight: bold;
    color: #edf1f5;
}
.empty-copy {
    display: block;
    margin-top: 12dp;
}
.empty-command {
    display: block;
    margin-top: 8dp;
    padding: 8dp;
    background-color: #0b0f14;
    color: #79c7d9;
}
#footer {
    display: block;
    position: absolute;
    left: 28dp;
    right: 28dp;
    bottom: 28dp;
    height: 44dp;
}
button {
    position: absolute;
    bottom: 0;
    width: 128dp;
    height: 42dp;
    border: 1dp #46525b;
    border-radius: 3dp;
    background-color: #252f38;
    color: #eef2f5;
    font-family: LatoLatin;
    font-size: 14dp;
    text-align: center;
    padding-top: 12dp;
    box-sizing: border-box;
}
button:hover {
    background-color: #31404a;
    border-color: #5a6872;
}
button.primary {
    right: 140dp;
    background-color: #2d7888;
    border-color: #58b8c9;
}
button.primary:hover {
    background-color: #348c9e;
}
button.disabled {
    background-color: #252b31;
    border-color: #343c43;
    color: #707a83;
}
#cancel-button {
    right: 0;
}
</style>
</head>
<body>
<div id="root">
    <div id="header">
        <div id="title">Select Engine</div>
        <div id="subtitle">Project: <span id="project-name">)"
             << escapeRmlText(projectName) << R"(</span></div>
    </div>
    <div id="engine-list">)"
             << buildEngineRows(engines) << R"(</div>
    <div id="footer">
        <button id="select-button" class="primary disabled">Select</button>
        <button id="cancel-button">Cancel</button>
    </div>
</div>
</body>
</rml>
)";
    return document.str();
}

bool parseEngineIndex(const std::string& id, int& index)
{
    constexpr const char* prefix = "engine-";
    constexpr size_t prefix_len = 7;
    if (id.rfind(prefix, 0) != 0)
        return false;

    try
    {
        index = std::stoi(id.substr(prefix_len));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

class EnginePickerListener final : public Rml::EventListener
{
public:
    EnginePickerListener(Rml::ElementDocument* document,
                         const std::vector<EngineEntry>& engines,
                         std::string& selectedId,
                         bool& done)
        : m_document(document)
        , m_engines(engines)
        , m_selectedId(selectedId)
        , m_done(done)
    {
    }

    void attach()
    {
        if (!m_document)
            return;

        m_document->AddEventListener(Rml::EventId::Keydown, this, true);

        if (auto* select = m_document->GetElementById("select-button"))
        {
            select->AddEventListener(Rml::EventId::Mousedown, this);
            select->AddEventListener(Rml::EventId::Click, this);
        }
        if (auto* cancel = m_document->GetElementById("cancel-button"))
        {
            cancel->AddEventListener(Rml::EventId::Mousedown, this);
            cancel->AddEventListener(Rml::EventId::Click, this);
        }

        for (size_t i = 0; i < m_engines.size(); ++i)
        {
            if (auto* row = m_document->GetElementById("engine-" + std::to_string(i)))
            {
                row->AddEventListener(Rml::EventId::Mousedown, this);
                row->AddEventListener(Rml::EventId::Click, this);
                row->AddEventListener(Rml::EventId::Dblclick, this);
            }
        }

        updateSelectButton();
    }

    void detach()
    {
        if (!m_document)
            return;

        m_document->RemoveEventListener(Rml::EventId::Keydown, this, true);

        if (auto* select = m_document->GetElementById("select-button"))
        {
            select->RemoveEventListener(Rml::EventId::Mousedown, this);
            select->RemoveEventListener(Rml::EventId::Click, this);
        }
        if (auto* cancel = m_document->GetElementById("cancel-button"))
        {
            cancel->RemoveEventListener(Rml::EventId::Mousedown, this);
            cancel->RemoveEventListener(Rml::EventId::Click, this);
        }

        for (size_t i = 0; i < m_engines.size(); ++i)
        {
            if (auto* row = m_document->GetElementById("engine-" + std::to_string(i)))
            {
                row->RemoveEventListener(Rml::EventId::Mousedown, this);
                row->RemoveEventListener(Rml::EventId::Click, this);
                row->RemoveEventListener(Rml::EventId::Dblclick, this);
            }
        }
    }

    void handlePointerPress(Rml::Element* element, bool double_click)
    {
        if (Rml::Element* action = resolveActionElement(element))
            processActionId(action->GetId(), double_click);
    }

    void handleListPointerPress(Rml::Vector2f point, bool double_click)
    {
        Rml::Element* list = m_document ? m_document->GetElementById("engine-list") : nullptr;
        Rml::Element* first_row = m_document ? m_document->GetElementById("engine-0") : nullptr;
        if (!list || !first_row || !list->IsPointWithinElement(point))
            return;

        const float row_height = first_row->GetOffsetHeight();
        if (row_height <= 0.0f)
            return;

        const float content_top = list->GetAbsoluteOffset(Rml::BoxArea::Padding).y;
        const float row_y = point.y - content_top + list->GetScrollTop();
        const int index = static_cast<int>(row_y / row_height);
        if (selectIndex(index) && double_click)
            confirmSelection();
    }

    void ProcessEvent(Rml::Event& event) override
    {
        if (event == Rml::EventId::Keydown)
        {
            processKeyDown(event);
            return;
        }

        Rml::Element* element = resolveActionElement(event.GetTargetElement());
        if (!element)
            element = resolveActionElement(event.GetCurrentElement());
        if (element)
        {
            processActionId(element->GetId(), event == Rml::EventId::Dblclick);
            event.StopPropagation();
        }
    }

private:
    Rml::Element* resolveActionElement(Rml::Element* element) const
    {
        while (element)
        {
            const std::string id = element->GetId();
            int engine_index = -1;
            if (id == "cancel-button" || id == "select-button" || parseEngineIndex(id, engine_index))
                return element;
            element = element->GetParentNode();
        }

        return nullptr;
    }

    void processActionId(const std::string& id, bool double_click)
    {
        if (id == "cancel-button")
        {
            m_done = true;
            return;
        }

        if (id == "select-button")
        {
            confirmSelection();
            return;
        }

        int engine_index = -1;
        if (parseEngineIndex(id, engine_index))
        {
            if (selectIndex(engine_index) && double_click)
                confirmSelection();
        }
    }

    bool selectIndex(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_engines.size()))
            return false;
        if (!m_engines[index].path_exists)
            return false;

        if (m_selectedIndex == index)
            return true;

        if (m_selectedIndex >= 0)
        {
            if (auto* old_row = m_document->GetElementById("engine-" + std::to_string(m_selectedIndex)))
                old_row->SetClass("selected", false);
        }

        m_selectedIndex = index;

        if (auto* new_row = m_document->GetElementById("engine-" + std::to_string(m_selectedIndex)))
            new_row->SetClass("selected", true);

        updateSelectButton();
        return true;
    }

    void selectRelative(int delta)
    {
        if (m_engines.empty())
            return;

        const int count = static_cast<int>(m_engines.size());
        int index = m_selectedIndex;
        if (index < 0)
            index = (delta > 0) ? -1 : count;

        for (int attempt = 0; attempt < count; ++attempt)
        {
            index = (index + delta + count) % count;
            if (selectIndex(index))
                return;
        }
    }

    void confirmSelection()
    {
        if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_engines.size()))
            return;
        if (!m_engines[m_selectedIndex].path_exists)
            return;

        m_selectedId = m_engines[m_selectedIndex].id;
        m_done = true;
    }

    void processKeyDown(Rml::Event& event)
    {
        const auto key = static_cast<Rml::Input::KeyIdentifier>(event.GetParameter<int>("key_identifier", 0));
        switch (key)
        {
            case Rml::Input::KI_ESCAPE:
                m_done = true;
                event.StopPropagation();
                break;
            case Rml::Input::KI_RETURN:
            case Rml::Input::KI_NUMPADENTER:
                confirmSelection();
                event.StopPropagation();
                break;
            case Rml::Input::KI_DOWN:
                selectRelative(1);
                event.StopPropagation();
                break;
            case Rml::Input::KI_UP:
                selectRelative(-1);
                event.StopPropagation();
                break;
            default:
                break;
        }
    }

    void updateSelectButton()
    {
        if (!m_document)
            return;

        if (auto* select = m_document->GetElementById("select-button"))
            select->SetClass("disabled", m_selectedIndex < 0);
    }

    Rml::ElementDocument* m_document = nullptr;
    const std::vector<EngineEntry>& m_engines;
    std::string& m_selectedId;
    bool& m_done;
    int m_selectedIndex = -1;
};
} // namespace

std::string showEnginePicker(const std::vector<EngineEntry>& engines, const std::string& projectName)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
        return "";
    }

    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    SDL_Window* window = SDL_CreateWindow(
        "Garden - Select Engine",
        620,
        460,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
    {
        fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
        SDL_Quit();
        return "";
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
    {
        fprintf(stderr, "SDL_CreateRenderer Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }
    SDL_SetRenderVSync(renderer, 1);

    SystemInterface_SDL system_interface;
    system_interface.SetWindow(window);
    RmlSdlRenderer render_interface(renderer);

    Rml::SetSystemInterface(&system_interface);
    Rml::SetRenderInterface(&render_interface);

    if (!Rml::Initialise())
    {
        fprintf(stderr, "RmlUi initialise failed.\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }

    int width = 620;
    int height = 460;
    SDL_GetCurrentRenderOutputSize(renderer, &width, &height);

    Rml::Context* context = Rml::CreateContext("garden_cli_engine_picker", Rml::Vector2i(width, height));
    if (!context)
    {
        fprintf(stderr, "RmlUi context creation failed.\n");
        Rml::Shutdown();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }

    updateDensityIndependentRatio(context, window);
    loadFonts(engines);

    const std::string rml = buildDocument(engines, projectName);
    Rml::ElementDocument* document = context->LoadDocumentFromMemory(rml, "garden_cli_engine_picker.rml");
    if (!document)
    {
        fprintf(stderr, "RmlUi failed to load GardenCLI engine picker document.\n");
        Rml::Shutdown();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return "";
    }

    document->Show();

    std::string selected_id;
    bool done = false;
    EnginePickerListener listener(document, engines, selected_id, done);
    listener.attach();

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

            processPickerEvent(context, window, renderer, event);
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                event.button.windowID == SDL_GetWindowID(window) &&
                event.button.button == SDL_BUTTON_LEFT)
            {
                const Rml::Vector2f render_position = toRenderCoordinates(renderer, event.button.x, event.button.y);
                listener.handlePointerPress(context->GetHoverElement(), event.button.clicks >= 2);
                listener.handleListPointerPress(render_position, event.button.clicks >= 2);
            }
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        context->Update();
        render_interface.beginFrame();
        context->Render();
        render_interface.endFrame();
        SDL_RenderPresent(renderer);
    }

    listener.detach();
    document->Close();
    context->Update();
    Rml::RemoveContext(context->GetName());
    Rml::Shutdown();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return selected_id;
}
