#include "HeadlessRenderAPI.hpp"
#include "HeadlessMesh.hpp"
#include "Graphics/RenderCommandBuffer.hpp"
#include "Components/camera.hpp"

HeadlessRenderAPI::HeadlessRenderAPI()
{
    m_lastFrameStats.backend_name = getAPIName();
}

HeadlessRenderAPI::~HeadlessRenderAPI()
{
}

bool HeadlessRenderAPI::initialize(WindowHandle window, int width, int height, float fov)
{
    return true;
}

void HeadlessRenderAPI::shutdown()
{
}

void HeadlessRenderAPI::resize(int width, int height)
{
}

void HeadlessRenderAPI::beginFrame()
{
    const char* name = m_lastFrameStats.backend_name;
    m_lastFrameStats = RenderFrameStats{};
    m_lastFrameStats.backend_name = (name && name[0] != '\0') ? name : getAPIName();
}

void HeadlessRenderAPI::endFrame()
{
}

void HeadlessRenderAPI::present()
{
}

void HeadlessRenderAPI::clear(const glm::vec3& color)
{
}

void HeadlessRenderAPI::setCamera(const camera& cam)
{
}

void HeadlessRenderAPI::pushMatrix()
{
}

void HeadlessRenderAPI::popMatrix()
{
}

void HeadlessRenderAPI::translate(const glm::vec3& pos)
{
}

void HeadlessRenderAPI::rotate(const glm::mat4& rotation)
{
}

void HeadlessRenderAPI::multiplyMatrix(const glm::mat4& matrix)
{
}

glm::mat4 HeadlessRenderAPI::getProjectionMatrix() const
{
    return glm::mat4(1.0f);
}

glm::mat4 HeadlessRenderAPI::getViewMatrix() const
{
    return glm::mat4(1.0f);
}

TextureHandle HeadlessRenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
{
    return 1; // Return a dummy handle
}

TextureHandle HeadlessRenderAPI::loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                                       bool flip_vertically, bool generate_mipmaps)
{
    return 1; // Return a dummy handle
}

void HeadlessRenderAPI::bindTexture(TextureHandle texture)
{
}

void HeadlessRenderAPI::unbindTexture()
{
}

void HeadlessRenderAPI::deleteTexture(TextureHandle texture)
{
}

void HeadlessRenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
}

void HeadlessRenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
}

void HeadlessRenderAPI::setRenderState(const RenderState& state)
{
}

void HeadlessRenderAPI::enableLighting(bool enable)
{
}

void HeadlessRenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
}

void HeadlessRenderAPI::renderSkybox()
{
}

void HeadlessRenderAPI::beginShadowPass(const glm::vec3& lightDir)
{
}

void HeadlessRenderAPI::beginShadowPass(const glm::vec3& lightDir, const camera& cam)
{
}

void HeadlessRenderAPI::beginCascade(int cascadeIndex)
{
}

void HeadlessRenderAPI::endShadowPass()
{
}

void HeadlessRenderAPI::bindShadowMap(int textureUnit)
{
}

glm::mat4 HeadlessRenderAPI::getLightSpaceMatrix()
{
    return glm::mat4(1.0f);
}

int HeadlessRenderAPI::getCascadeCount() const
{
    return 4;
}

const float* HeadlessRenderAPI::getCascadeSplitDistances() const
{
    static float distances[5] = { 0.1f, 10.0f, 35.0f, 90.0f, 200.0f };
    return distances;
}

const glm::mat4* HeadlessRenderAPI::getLightSpaceMatrices() const
{
    static glm::mat4 matrices[4] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
    return matrices;
}

IGPUMesh* HeadlessRenderAPI::createMesh()
{
    return new HeadlessMesh();
}

void HeadlessRenderAPI::replayCommandBuffer(const RenderCommandBuffer& cmds)
{
    if (cmds.empty()) return;

    m_lastFrameStats.submitted_draw_commands += cmds.size();
    m_lastFrameStats.backend_name = getAPIName();

    // Fresh-list semantics: every replay starts unbound, mirroring the real
    // backends after a command-list reset.
    bool havePSO = false;
    PSOKey lastPSO;
    bool haveTexture = false;
    TextureHandle lastTexture = INVALID_TEXTURE;
    const IGPUMesh* lastMesh = nullptr;

    for (const auto& cmd : cmds)
    {
        if (!cmd.gpu_mesh || !cmd.gpu_mesh->isUploaded()) continue;

        if (!havePSO || cmd.pso_key != lastPSO)
        {
            lastPSO = cmd.pso_key;
            havePSO = true;
        }
        else
        {
            m_lastFrameStats.state_binds_skipped++;
        }

        TextureHandle tex = cmd.use_texture ? cmd.texture : INVALID_TEXTURE;
        if (!haveTexture || tex != lastTexture)
        {
            lastTexture = tex;
            haveTexture = true;
        }
        else
        {
            m_lastFrameStats.state_binds_skipped++;
        }

        if (cmd.gpu_mesh != lastMesh)
        {
            lastMesh = cmd.gpu_mesh;
        }
        else
        {
            m_lastFrameStats.state_binds_skipped++;
        }

        m_lastFrameStats.backend_draw_calls++;
    }
}

RenderFrameStats HeadlessRenderAPI::getLastFrameStats() const
{
    return m_lastFrameStats;
}
