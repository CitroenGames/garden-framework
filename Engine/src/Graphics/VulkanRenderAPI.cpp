#include "VulkanRenderAPI.hpp"
#include "VulkanMesh.hpp"
#include <stdio.h>

VulkanRenderAPI::VulkanRenderAPI()
{
}

VulkanRenderAPI::~VulkanRenderAPI()
{
}

bool VulkanRenderAPI::initialize(WindowHandle window, int width, int height, float fov)
{
    printf("VulkanRenderAPI::initialize - NOT IMPLEMENTED\n");
    return true;
}

void VulkanRenderAPI::shutdown()
{
    printf("VulkanRenderAPI::shutdown\n");
}

void VulkanRenderAPI::resize(int width, int height)
{
}

void VulkanRenderAPI::beginFrame()
{
}

void VulkanRenderAPI::endFrame()
{
}

void VulkanRenderAPI::present()
{
}

void VulkanRenderAPI::clear(const vector3f& color)
{
}

void VulkanRenderAPI::setCamera(const camera& cam)
{
}

void VulkanRenderAPI::pushMatrix()
{
}

void VulkanRenderAPI::popMatrix()
{
}

void VulkanRenderAPI::translate(const vector3f& pos)
{
}

void VulkanRenderAPI::rotate(const matrix4f& rotation)
{
}

void VulkanRenderAPI::multiplyMatrix(const matrix4f& matrix)
{
}

TextureHandle VulkanRenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
{
    printf("VulkanRenderAPI::loadTexture not implemented\n");
    return INVALID_TEXTURE;
}

void VulkanRenderAPI::bindTexture(TextureHandle texture)
{
}

void VulkanRenderAPI::unbindTexture()
{
}

void VulkanRenderAPI::deleteTexture(TextureHandle texture)
{
}

void VulkanRenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    // printf("VulkanRenderAPI::renderMesh not implemented\n");
}

void VulkanRenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
}

void VulkanRenderAPI::setRenderState(const RenderState& state)
{
}

void VulkanRenderAPI::enableLighting(bool enable)
{
}

void VulkanRenderAPI::setLighting(const vector3f& ambient, const vector3f& diffuse, const vector3f& direction)
{
}

void VulkanRenderAPI::renderSkybox()
{
}

void VulkanRenderAPI::beginShadowPass(const vector3f& lightDir)
{
}

void VulkanRenderAPI::endShadowPass()
{
}

void VulkanRenderAPI::bindShadowMap(int textureUnit)
{
}

matrix4f VulkanRenderAPI::getLightSpaceMatrix()
{
    return matrix4f();
}

IGPUMesh* VulkanRenderAPI::createMesh()
{
    return new VulkanMesh();
}
