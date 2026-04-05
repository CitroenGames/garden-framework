#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <string>

struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;
struct VmaAllocation_T;
typedef VmaAllocation_T* VmaAllocation;

class VulkanRenderAPI;

class RmlRenderer_VK : public Rml::RenderInterface {
public:
    RmlRenderer_VK();
    ~RmlRenderer_VK();

    bool Init(VulkanRenderAPI* renderAPI);
    void Shutdown();

    void SetViewport(int width, int height);

    // Called each frame to sync with current command buffer
    void BeginFrame();

    // -- Rml::RenderInterface --
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    bool CreatePipelines();
    bool CreateDescriptorResources();

    std::vector<char> ReadShaderFile(const std::string& path);
    VkShaderModule CreateShaderModule(const std::vector<char>& code);

    VulkanRenderAPI* m_renderAPI = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;

    // Pipeline
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipelineTextured = VK_NULL_HANDLE;
    VkPipeline m_pipelineColor = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;

    // Descriptor pool for texture bindings
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

    // Sampler
    VkSampler m_sampler = VK_NULL_HANDLE;

    // Push constant data matching Slang RmlCB (80 bytes)
    struct RmlUBO {
        float transform[16]; // mat4
        float translation[2]; // vec2
        float _pad[2];        // padding to match float4x4 + float2 + float2
    };

    // Default descriptor set for color-only (no texture) rendering
    VkDescriptorSet m_colorOnlyDescriptorSet = VK_NULL_HANDLE;
    VkImage m_dummyImage = VK_NULL_HANDLE;
    VmaAllocation m_dummyAllocation = nullptr;
    VkImageView m_dummyImageView = VK_NULL_HANDLE;

    // Geometry storage
    struct GeometryData {
        VkBuffer vertexBuffer;
        VmaAllocation vertexAlloc;
        VkBuffer indexBuffer;
        VmaAllocation indexAlloc;
        int numIndices;
    };
    uintptr_t m_nextGeometryHandle = 1;
    std::unordered_map<uintptr_t, GeometryData> m_geometries;

    // Texture storage
    struct TextureData {
        VkImage image;
        VmaAllocation allocation;
        VkImageView imageView;
        VkDescriptorSet descriptorSet;
    };
    uintptr_t m_nextTextureHandle = 1;
    std::unordered_map<uintptr_t, TextureData> m_textures;

    // State
    int m_viewportWidth = 0;
    int m_viewportHeight = 0;
    bool m_scissorEnabled = false;
    bool m_transformEnabled = false;
    Rml::Matrix4f m_transform;

    VkCommandBuffer m_currentCmdBuffer = VK_NULL_HANDLE;
};
