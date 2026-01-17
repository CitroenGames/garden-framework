#include "HeadlessRenderAPI.hpp"
#include "HeadlessMesh.hpp"

HeadlessRenderAPI::HeadlessRenderAPI()
{
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

TextureHandle HeadlessRenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
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

IGPUMesh* HeadlessRenderAPI::createMesh()
{
    return new HeadlessMesh();
}
