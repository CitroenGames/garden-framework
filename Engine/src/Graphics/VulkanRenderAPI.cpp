#include "VulkanRenderAPI.hpp"
#include "VulkanMesh.hpp"
#include "Components/camera.hpp"
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

void VulkanRenderAPI::clear(const glm::vec3& color)
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

void VulkanRenderAPI::translate(const glm::vec3& pos)
{
}

void VulkanRenderAPI::rotate(const glm::mat4& rotation)
{
}

void VulkanRenderAPI::multiplyMatrix(const glm::mat4& matrix)
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

void VulkanRenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
}

void VulkanRenderAPI::renderSkybox()
{
}

void VulkanRenderAPI::beginShadowPass(const glm::vec3& lightDir)
{
}

void VulkanRenderAPI::beginShadowPass(const glm::vec3& lightDir, const camera& cam)
{
}

void VulkanRenderAPI::beginCascade(int cascadeIndex)
{
}

void VulkanRenderAPI::endShadowPass()
{
}

void VulkanRenderAPI::bindShadowMap(int textureUnit)
{
}

glm::mat4 VulkanRenderAPI::getLightSpaceMatrix()
{
    return glm::mat4(1.0f);
}

int VulkanRenderAPI::getCascadeCount() const
{
    return 4;
}

const float* VulkanRenderAPI::getCascadeSplitDistances() const
{
    static float distances[5] = { 0.1f, 10.0f, 35.0f, 90.0f, 200.0f };
    return distances;
}

const glm::mat4* VulkanRenderAPI::getLightSpaceMatrices() const
{
    static glm::mat4 matrices[4] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
    return matrices;
}

IGPUMesh* VulkanRenderAPI::createMesh()
{
    return new VulkanMesh();
}
