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

void HeadlessRenderAPI::clear(const vector3f& color)
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

void HeadlessRenderAPI::translate(const vector3f& pos)
{
}

void HeadlessRenderAPI::rotate(const matrix4f& rotation)
{
}

void HeadlessRenderAPI::multiplyMatrix(const matrix4f& matrix)
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

void HeadlessRenderAPI::setLighting(const vector3f& ambient, const vector3f& diffuse, const vector3f& direction)
{
}

void HeadlessRenderAPI::renderSkybox()
{
}

void HeadlessRenderAPI::beginShadowPass(const vector3f& lightDir)
{
}

void HeadlessRenderAPI::endShadowPass()
{
}

void HeadlessRenderAPI::bindShadowMap(int textureUnit)
{
}

matrix4f HeadlessRenderAPI::getLightSpaceMatrix()
{
    return matrix4f();
}

IGPUMesh* HeadlessRenderAPI::createMesh()
{
    return new HeadlessMesh();
}
