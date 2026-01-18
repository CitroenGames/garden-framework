#pragma once

#include "RenderAPI.hpp"
#include <cstdint>
#include <stack>
#include <vector>
#include <unordered_map>
#include <string>
#include <array>

// Vulkan headers
#include <vulkan/vulkan.h>

// vk-bootstrap
#include "VkBootstrap.h"

// VMA forward declaration
struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;
struct VmaAllocation_T;
typedef VmaAllocation_T* VmaAllocation;

// Forward declarations
class VulkanMesh;

// Vulkan texture structure
struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;

    bool isValid() const { return image != VK_NULL_HANDLE; }
};

// Global UBO structure (matches shader layout)
struct GlobalUBO {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 lightDir;
    float _pad1;
    glm::vec3 lightAmbient;
    float _pad2;
    glm::vec3 lightDiffuse;
    float _pad3;
    glm::vec3 color;
    int useTexture;
};

class VulkanRenderAPI : public IRenderAPI
{
public:
    VulkanRenderAPI();
    virtual ~VulkanRenderAPI();

    // IRenderAPI implementation
    virtual bool initialize(WindowHandle window, int width, int height, float fov) override;
    virtual void shutdown() override;
    virtual void resize(int width, int height) override;

    virtual void beginFrame() override;
    virtual void endFrame() override;
    virtual void present() override;
    virtual void clear(const glm::vec3& color = glm::vec3(0.2f, 0.3f, 0.8f)) override;

    virtual void setCamera(const camera& cam) override;
    virtual void pushMatrix() override;
    virtual void popMatrix() override;
    virtual void translate(const glm::vec3& pos) override;
    virtual void rotate(const glm::mat4& rotation) override;
    virtual void multiplyMatrix(const glm::mat4& matrix) override;

    virtual TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) override;
    virtual TextureHandle loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                                bool flip_vertically = false, bool generate_mipmaps = true) override;
    virtual void bindTexture(TextureHandle texture) override;
    virtual void unbindTexture() override;
    virtual void deleteTexture(TextureHandle texture) override;

    virtual void renderMesh(const mesh& m, const RenderState& state = RenderState()) override;
    virtual void renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state = RenderState()) override;

    virtual void setRenderState(const RenderState& state) override;
    virtual void enableLighting(bool enable) override;
    virtual void setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction) override;

    virtual void renderSkybox() override;

    // Shadow Mapping overrides (CSM) - stub implementations for MVP
    virtual void beginShadowPass(const glm::vec3& lightDir) override;
    virtual void beginShadowPass(const glm::vec3& lightDir, const camera& cam) override;
    virtual void beginCascade(int cascadeIndex) override;
    virtual void endShadowPass() override;
    virtual void bindShadowMap(int textureUnit) override;
    virtual glm::mat4 getLightSpaceMatrix() override;
    virtual int getCascadeCount() const override;
    virtual const float* getCascadeSplitDistances() const override;
    virtual const glm::mat4* getLightSpaceMatrices() const override;

    virtual IGPUMesh* createMesh() override;

    virtual const char* getAPIName() const override { return "Vulkan"; }

    // Vulkan-specific accessors for VulkanMesh
    VkDevice getDevice() const { return device; }
    VmaAllocator getAllocator() const { return vma_allocator; }
    VkCommandPool getCommandPool() const { return command_pool; }
    VkQueue getGraphicsQueue() const { return graphics_queue; }

private:
    // Initialization helpers
    bool createInstance();
    bool createSurface();
    bool selectPhysicalDevice();
    bool createLogicalDevice();
    bool createVmaAllocator();
    bool createSwapchain();
    bool createImageViews();
    bool createDepthResources();
    bool createRenderPass();
    bool createFramebuffers();
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    bool createDescriptorSetLayout();
    bool createGraphicsPipeline();
    bool createDescriptorPool();
    bool createUniformBuffers();
    bool createDescriptorSets();
    bool createDefaultTexture();

    void cleanupSwapchain();
    void recreateSwapchain();

    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> readShaderFile(const std::string& filename);

    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               uint32_t mipLevels = 1);
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    void generateMipmaps(VkImage image, VkFormat imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels);

    void updateDescriptorSet(uint32_t frameIndex, TextureHandle texture);

private:
    // vk-bootstrap handles
    vkb::Instance vkb_instance;
    vkb::PhysicalDevice vkb_physical_device;
    vkb::Device vkb_device;

    // Core Vulkan handles
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    uint32_t graphics_queue_family = 0;

    // Swapchain
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent = {0, 0};
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchain_image_views;

    // Depth buffer
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageView depth_image_view = VK_NULL_HANDLE;
    VmaAllocation depth_image_allocation = nullptr;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

    // Render pass and framebuffers
    VkRenderPass render_pass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;

    // Command pools and buffers
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers;

    // Synchronization
    static const int MAX_FRAMES_IN_FLIGHT = 2;
    std::vector<VkSemaphore> image_available_semaphores;
    std::vector<VkSemaphore> render_finished_semaphores;
    std::vector<VkFence> in_flight_fences;
    uint32_t current_frame = 0;
    uint32_t current_image_index = 0;

    // VMA allocator
    VmaAllocator vma_allocator = nullptr;

    // Pipeline
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline graphics_pipeline = VK_NULL_HANDLE;

    // Descriptors
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptor_sets;

    // Uniform buffers (per-frame)
    std::vector<VkBuffer> uniform_buffers;
    std::vector<VmaAllocation> uniform_buffer_allocations;
    std::vector<void*> uniform_buffer_mapped;

    // Matrix stack (CPU-side)
    glm::mat4 projection_matrix = glm::mat4(1.0f);
    glm::mat4 view_matrix = glm::mat4(1.0f);
    glm::mat4 current_model_matrix = glm::mat4(1.0f);
    std::stack<glm::mat4> model_matrix_stack;

    // Lighting state
    glm::vec3 light_direction = glm::vec3(0.0f, -1.0f, 0.0f);
    glm::vec3 light_ambient = glm::vec3(0.2f);
    glm::vec3 light_diffuse = glm::vec3(0.8f);
    bool lighting_enabled = true;

    // Texture management
    std::unordered_map<TextureHandle, VulkanTexture> textures;
    TextureHandle next_texture_handle = 1;
    TextureHandle bound_texture = INVALID_TEXTURE;
    VulkanTexture default_texture;

    // Window/viewport state
    WindowHandle window_handle = nullptr;
    int viewport_width = 0;
    int viewport_height = 0;
    float field_of_view = 75.0f;

    // Current render state
    RenderState current_state;
    bool frame_started = false;

    // Clear color (set by clear(), used in beginFrame)
    glm::vec3 clear_color = glm::vec3(0.2f, 0.3f, 0.8f);

    // CSM stub data
    static const int NUM_CASCADES = 4;
    float cascadeSplitDistances[5] = { 0.1f, 10.0f, 35.0f, 90.0f, 200.0f };
    glm::mat4 lightSpaceMatrices[4] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };

#ifdef _DEBUG
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
#endif
};
