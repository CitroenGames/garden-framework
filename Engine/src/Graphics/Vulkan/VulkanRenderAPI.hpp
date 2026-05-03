#pragma once

#include "Graphics/RenderAPI.hpp"
#include "Graphics/RenderCommandBuffer.hpp"
#include "VulkanTypes.hpp"
#include "VkDeletionQueue.hpp"
#include "VkSamplerCache.hpp"
#include "VulkanPostProcessPass.hpp"
#include "VulkanGBufferPass.hpp"
#include "VulkanDeferredLightingPass.hpp"
#include "VulkanRGBackend.hpp"
#include "VulkanPostProcessGraphBuilder.hpp"
#include "Graphics/RenderGraph/RenderGraph.hpp"
#include "VulkanSceneViewport.hpp"
#include <cstdint>
#include <memory>
#include <stack>
#include <vector>
#include <unordered_map>
#include <string>
#include <array>
#include <atomic>
#include <mutex>
#include <future>

// vk-bootstrap
#include "VkBootstrap.h"

// Forward declarations
class VulkanMesh;

class VulkanRenderAPI : public IRenderAPI
{
    friend class VulkanPostProcessGraphBuilder;
    friend class VulkanSceneViewport;

public:
    VulkanRenderAPI();
    virtual ~VulkanRenderAPI();

    // IRenderAPI implementation
    virtual bool initialize(WindowHandle window, int width, int height, float fov) override;
    virtual void shutdown() override;
    virtual void waitForGPU() override;
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

    virtual glm::mat4 getProjectionMatrix() const override;
    virtual glm::mat4 getViewMatrix() const override;

    virtual TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) override;
    virtual TextureHandle loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                                bool flip_vertically = false, bool generate_mipmaps = true) override;
    virtual TextureHandle loadCompressedTexture(int width, int height, uint32_t format, int mip_count,
                                                const std::vector<const uint8_t*>& mip_data,
                                                const std::vector<size_t>& mip_sizes,
                                                const std::vector<std::pair<int,int>>& mip_dimensions) override;
    virtual void bindTexture(TextureHandle texture) override;
    virtual void unbindTexture() override;
    virtual void deleteTexture(TextureHandle texture) override;

    virtual void renderMesh(const mesh& m, const RenderState& state = RenderState()) override;
    virtual void renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state = RenderState()) override;

    virtual void setRenderState(const RenderState& state) override;
    virtual void enableLighting(bool enable) override;
    virtual void setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction) override;
    virtual void setPointAndSpotLights(const LightCBuffer& lights) override;

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

    // Command buffer replay (multicore rendering)
    virtual void replayCommandBuffer(const RenderCommandBuffer& cmds) override;
    virtual void replayCommandBufferParallel(const RenderCommandBuffer& cmds) override;

    // Debug line rendering
    virtual void renderDebugLines(const vertex* vertices, size_t vertex_count) override;

    virtual const char* getAPIName() const override { return "Vulkan"; }

    // Graphics settings
    virtual void setFXAAEnabled(bool enabled) override;
    virtual bool isFXAAEnabled() const override;
    virtual void setShadowQuality(int quality) override;
    virtual int getShadowQuality() const override;
    virtual void setShadowCascadeCount(int count) override;
    virtual void setSSAOEnabled(bool enabled) override;
    virtual bool isSSAOEnabled() const override;
    virtual void setSSAORadius(float radius) override;
    virtual void setSSAOIntensity(float intensity) override;

    void setDeferredEnabled(bool enabled) override { m_useDeferred = enabled; }
    bool isDeferredEnabled() const override { return m_useDeferred; }
    bool isDeferredActive() const override;
    void submitDeferredOpaqueCommands(const RenderCommandBuffer& cmds) override;
    void submitDeferredTransparentCommands(const RenderCommandBuffer& cmds) override;
    void uploadLightBuffers(const GPUPointLight* pts, int ptCount,
                            const GPUSpotLight* spts, int spCount) override;

    // Vulkan-specific accessors for VulkanMesh
    VkDevice getDevice() const { return device; }
    VmaAllocator getAllocator() const { return vma_allocator; }
    VkCommandPool getCommandPool() const { return command_pool; }
    VkQueue getGraphicsQueue() const { return graphics_queue; }

    // ImGui accessors
    VkInstance getInstance() const { return instance; }
    VkPhysicalDevice getPhysicalDevice() const { return physical_device; }
    uint32_t getGraphicsQueueFamily() const { return graphics_queue_family; }
    VkRenderPass getRenderPass() const { return render_pass; }
    VkRenderPass getFxaaRenderPass() const { return fxaaPass_.getRenderPass(); }
    uint32_t getSwapchainImageCount() const { return static_cast<uint32_t>(swapchain_images.size()); }
    VkCommandBuffer getCurrentCommandBuffer() const { return command_buffers[current_frame]; }
    VkFormat getSwapchainFormat() const { return swapchain_format; }
    uint32_t getCurrentFrameIndex() const { return current_frame; }
    VkDeletionQueue& getDeletionQueue() { return deletion_queue; }

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
    bool ensureRenderFinishedSemaphores();
    bool createDescriptorSetLayout();
    bool createGraphicsPipeline();
    bool createDescriptorPool();
    bool createUniformBuffers();
    bool createDescriptorSets();
    bool createDefaultTexture();

    // Frame preparation (fence, acquire, command buffer begin)
    void prepareFrame();

    // Shadow mapping helpers
    bool createShadowResources();
    void cleanupShadowResources();
    void recreateShadowResources(uint32_t size);
    void calculateCascadeSplits(float nearPlane, float farPlane);
    std::array<glm::vec3, 8> getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view);
    glm::mat4 getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
        const glm::mat4& viewMatrix, float fov, float aspect);

    // Skybox helpers
    bool createSkyboxResources();
    void cleanupSkyboxResources();

    // Post-processing helpers
    bool createPostProcessingResources();
    void cleanupPostProcessingResources();
    void recreateOffscreenResources();
    void renderFXAAPass(VkCommandBuffer cmd,
                        VkRenderPass renderPass, VkFramebuffer framebuffer,
                        VkPipeline pipeline,
                        uint32_t width, uint32_t height,
                        bool enableSSAO, bool enableShadowMask,
                        bool renderImGui);
    void uploadLightUniformBuffer(uint32_t frameIndex);
    void uploadCurrentForwardLightBuffers(uint32_t frameIndex);
    void renderDebugLinesDirect(const vertex* vertices, size_t vertex_count);

    void cleanupSwapchain();
    void recreateSwapchain();

    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> readShaderFile(const std::string& filename);

    // Pipeline selection based on render state (lighting, blend mode, cull mode)
    VkPipeline selectPipeline(const RenderState& state) const;

    // Pipeline cache persistence
    bool loadPipelineCache();
    void savePipelineCache();

    // Shared staging buffer
    void ensureStagingBuffer(VkDeviceSize requiredSize);

    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               uint32_t mipLevels = 1);
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    void generateMipmaps(VkImage image, VkFormat imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels);

    VkDescriptorSet getOrAllocateDescriptorSet(uint32_t frameIndex, TextureHandle texture);
    VkDescriptorPool createPerDrawDescriptorPool();
    VkDescriptorSet allocateFromPerDrawPool(uint32_t frameIndex);
    void initializeDescriptorSet(VkDescriptorSet ds, uint32_t frameIndex, TextureHandle texture);

    // Parallel replay helpers (declarations that don't reference PerThreadCommandPool)
    bool createContinuationRenderPass();
    bool createThreadCommandPools();
    void restoreDynamicState(VkCommandBuffer cmd, VkExtent2D extent);
    // Remaining parallel replay helpers declared after PerThreadCommandPool definition below

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

    // Synchronization constants (needed before PerThreadCommandPool)
    static const int MAX_FRAMES_IN_FLIGHT = 2;
    static constexpr uint64_t FENCE_TIMEOUT_NS = 5'000'000'000ULL; // 5 seconds

    // Per-draw descriptor pool state (needed before PerThreadCommandPool)
    static constexpr uint32_t SETS_PER_POOL = 512;
    struct PerFrameDescriptorState {
        std::vector<VkDescriptorPool> pools;
        uint32_t current_pool = 0;
        uint32_t sets_allocated_in_pool = 0;
    };

    // Per-thread command pools for parallel secondary command buffer recording.
    // Vulkan command pools are NOT thread-safe, so each worker thread needs its own.
    // Secondary command buffers are recorded with RENDER_PASS_CONTINUE_BIT and
    // executed from the primary command buffer via vkCmdExecuteCommands().
    struct PerThreadCommandPool {
        VkCommandPool pool[MAX_FRAMES_IN_FLIGHT] = {};
        VkCommandBuffer secondary_buffer[MAX_FRAMES_IN_FLIGHT] = {};
        std::atomic<bool> in_use{false};

        // Per-worker descriptor allocation (avoids contention on shared pools)
        PerFrameDescriptorState descriptor_state[MAX_FRAMES_IN_FLIGHT] = {};
        std::unordered_map<TextureHandle, VkDescriptorSet> texture_cache;

        PerThreadCommandPool() = default;
        PerThreadCommandPool(PerThreadCommandPool&& o) noexcept
        {
            for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
                pool[f] = o.pool[f]; o.pool[f] = VK_NULL_HANDLE;
                secondary_buffer[f] = o.secondary_buffer[f]; o.secondary_buffer[f] = VK_NULL_HANDLE;
                descriptor_state[f] = std::move(o.descriptor_state[f]);
            }
            in_use.store(o.in_use.load(std::memory_order_relaxed), std::memory_order_relaxed);
            texture_cache = std::move(o.texture_cache);
        }
        PerThreadCommandPool& operator=(PerThreadCommandPool&& o) noexcept {
            for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
                pool[f] = o.pool[f]; o.pool[f] = VK_NULL_HANDLE;
                secondary_buffer[f] = o.secondary_buffer[f]; o.secondary_buffer[f] = VK_NULL_HANDLE;
                descriptor_state[f] = std::move(o.descriptor_state[f]);
            }
            in_use.store(o.in_use.load(std::memory_order_relaxed), std::memory_order_relaxed);
            texture_cache = std::move(o.texture_cache);
            return *this;
        }
        PerThreadCommandPool(const PerThreadCommandPool&) = delete;
        PerThreadCommandPool& operator=(const PerThreadCommandPool&) = delete;
    };
    std::vector<PerThreadCommandPool> m_threadCommandPools;

    // Parallel replay helpers that reference PerThreadCommandPool
    PerThreadCommandPool* acquireThreadPool();
    VkDescriptorSet workerAllocateFromPool(PerThreadCommandPool& worker, uint32_t frameIndex);
    VkDescriptorSet workerGetOrAllocateDescriptorSet(PerThreadCommandPool& worker, uint32_t frameIndex, TextureHandle texture);

    // Continuation render pass for parallel replay (loadOp=LOAD to preserve prior content)
    VkRenderPass continuation_render_pass = VK_NULL_HANDLE;
    static constexpr size_t VK_PARALLEL_REPLAY_THRESHOLD = 512;

    // Cached framebuffer state from beginFrame() for use in replayCommandBufferParallel()
    VkFramebuffer current_active_framebuffer = VK_NULL_HANDLE;
    VkExtent2D current_render_extent = {0, 0};
    // Color image backing current_active_framebuffer's first attachment.
    // Used by parallel-replay's render-pass-split barrier and endFrame's
    // post-continuation barrier — these need to transition the layout of
    // the actual bound attachment, which can be the legacy offscreen_image,
    // the legacy viewport_image, or a PIE viewport's image (the latter when
    // the editor's main viewport routes through a SceneViewport wrapper).
    VkImage current_active_color_image = VK_NULL_HANDLE;
    bool using_continuation_pass = false;

    // Synchronization
    std::vector<VkSemaphore> image_available_semaphores;
    std::vector<VkSemaphore> render_finished_semaphores;
    std::vector<VkFence> in_flight_fences;
    uint32_t current_frame = 0;
    uint32_t current_image_index = 0;

    // VMA allocator
    VmaAllocator vma_allocator = nullptr;

    // Deferred deletion queue
    VkDeletionQueue deletion_queue;

    // Sampler cache
    VkSamplerCache sampler_cache;

    // Disk-persisted pipeline cache
    VkPipelineCache vk_pipeline_cache = VK_NULL_HANDLE;

    // Redundant bind tracking
    VkPipeline last_bound_pipeline = VK_NULL_HANDLE;
    VkDescriptorSet last_bound_descriptor_set = VK_NULL_HANDLE;
    VkBuffer last_bound_vertex_buffer = VK_NULL_HANDLE;
    uint32_t last_bound_dynamic_offset = UINT32_MAX;

    // Shared staging buffer (guarded by staging_mutex for thread safety)
    std::mutex staging_mutex;
    static constexpr VkDeviceSize STAGING_BUFFER_INITIAL_SIZE = 64 * 1024 * 1024; // 64 MB
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VmaAllocation staging_allocation = nullptr;
    void* staging_mapped = nullptr;
    VkDeviceSize staging_capacity = 0;

    // Pipeline
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline graphics_pipeline = VK_NULL_HANDLE;

    // Lit pipeline variants (basic shader)
    VkPipeline pipeline_lit_noblend_cullback = VK_NULL_HANDLE;
    VkPipeline pipeline_lit_noblend_cullfront = VK_NULL_HANDLE;
    VkPipeline pipeline_lit_noblend_cullnone = VK_NULL_HANDLE;
    VkPipeline pipeline_lit_alpha_cullback = VK_NULL_HANDLE;
    VkPipeline pipeline_lit_alpha_cullnone = VK_NULL_HANDLE;
    VkPipeline pipeline_lit_additive = VK_NULL_HANDLE;

    // Unlit pipeline variants (unlit shader)
    VkPipeline pipeline_unlit_noblend_cullback = VK_NULL_HANDLE;
    VkPipeline pipeline_unlit_noblend_cullnone = VK_NULL_HANDLE;
    VkPipeline pipeline_unlit_alpha_cullback = VK_NULL_HANDLE;
    VkPipeline pipeline_unlit_alpha_cullnone = VK_NULL_HANDLE;
    VkPipeline pipeline_unlit_additive = VK_NULL_HANDLE;

    // Debug line pipeline (unlit shader, LINE_LIST topology)
    VkPipeline pipeline_debug_lines = VK_NULL_HANDLE;

    // Debug line vertex buffer (CPU-visible, recreated per frame)
    VkBuffer debug_line_buffer = VK_NULL_HANDLE;
    VmaAllocation debug_line_allocation = nullptr;
    void* debug_line_mapped = nullptr;
    size_t debug_line_buffer_capacity = 0;

    // Descriptors
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;

    // Per-frame global descriptor sets (UBO-only, on a dedicated pool)
    VkDescriptorPool global_descriptor_pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptor_sets;

    // Per-draw descriptor pools (dynamically growing, reset each frame).
    // Structured as a per-context allocator for future multicore rendering:
    // each parallel recording context can get its own VulkanDescriptorContext
    // to eliminate contention on descriptor allocation.

    // A self-contained descriptor allocation context.
    // For single-threaded use, one context per frame is used (context index 0).
    // For multicore rendering, additional contexts can be created per worker thread.
    struct VulkanDescriptorContext {
        PerFrameDescriptorState state[2]; // One per frame in flight
        std::unordered_map<TextureHandle, VkDescriptorSet> texture_cache;
        bool limit_warned = false;

        void reset(uint32_t frame_index) {
            auto& s = state[frame_index];
            for (auto pool : s.pools) {
                vkResetDescriptorPool(VK_NULL_HANDLE, pool, 0); // Device set at reset time
            }
            s.current_pool = 0;
            s.sets_allocated_in_pool = 0;
            texture_cache.clear();
        }
    };

    // Primary descriptor context (used for single-threaded and as pool[0] for multicore)
    PerFrameDescriptorState frame_descriptor_state[2]; // MAX_FRAMES_IN_FLIGHT
    bool descriptor_limit_warned = false;

    // Cache: texture handle -> VkDescriptorSet (for reuse within a frame)
    std::unordered_map<TextureHandle, VkDescriptorSet> texture_descriptor_cache;

    // Uniform buffers (per-frame) - GlobalUBO at binding 0
    std::vector<VkBuffer> uniform_buffers;
    std::vector<VmaAllocation> uniform_buffer_allocations;
    std::vector<void*> uniform_buffer_mapped;

    // Light UBO buffers (per-frame) - LightUBO at binding 3
    std::vector<VkBuffer> light_uniform_buffers;
    std::vector<VmaAllocation> light_uniform_allocations;
    std::vector<void*> light_uniform_mapped;

    // Dummy zero-filled SSBO fallback for bindings 10/11 when real light
    // buffers are not available yet.
    VkBuffer m_dummy_lights_buffer = VK_NULL_HANDLE;
    VmaAllocation m_dummy_lights_allocation = VK_NULL_HANDLE;

    // Per-frame uniform buffer for the DeferredLightingCB. Sized for one CB
    // (~640 bytes); NUM_FRAMES_IN_FLIGHT=2 instances so consecutive frames
    // can write without racing the GPU.
    std::vector<VkBuffer> m_deferred_lighting_cb_buffers;
    std::vector<VmaAllocation> m_deferred_lighting_cb_allocations;
    std::vector<void*> m_deferred_lighting_cb_mapped;

    // Per-frame point / spot light SSBOs for the deferred lighting pass.
    // Sized for MAX_LIGHTS_DEFERRED entries; populated by uploadLightBuffers.
    static constexpr int MAX_LIGHTS_DEFERRED = 256;
    std::vector<VkBuffer> m_point_lights_buffers;
    std::vector<VmaAllocation> m_point_lights_allocations;
    std::vector<void*> m_point_lights_mapped;
    std::vector<VkBuffer> m_spot_lights_buffers;
    std::vector<VmaAllocation> m_spot_lights_allocations;
    std::vector<void*> m_spot_lights_mapped;
    int m_num_point_lights_deferred = 0;
    int m_num_spot_lights_deferred  = 0;

    // Per-object dynamic UBO ring buffer (per-frame) - PerObjectUBO at binding 4
    static constexpr uint32_t MAX_PER_OBJECT_DRAWS = 16384;
    VkDeviceSize per_object_alignment = 0; // minUniformBufferOffsetAlignment-aligned size
    std::vector<VkBuffer> per_object_uniform_buffers;
    std::vector<VmaAllocation> per_object_uniform_allocations;
    std::vector<void*> per_object_uniform_mapped;
    std::atomic<uint32_t> per_object_draw_index[2] = {0, 0}; // per-frame draw counter (atomic for multicore)

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
    LightCBuffer current_lights{};

    // Texture management
    std::unordered_map<TextureHandle, VulkanTexture> textures;
    TextureHandle next_texture_handle = 1;
    TextureHandle bound_texture = INVALID_TEXTURE;
    VulkanTexture default_texture;

    // Default PBR textures (1x1 each, used when no PBR maps are provided)
    VulkanTexture default_normal_texture;              // flat normal (128,128,255,255)
    VulkanTexture default_metallic_roughness_texture;  // metallic=0, roughness=0.5
    VulkanTexture default_occlusion_texture;           // no occlusion (white)
    VulkanTexture default_emissive_texture;            // no emission (black)

    // Default shadow fallback (1x1 depth texture + comparison sampler)
    VkImage default_shadow_image = VK_NULL_HANDLE;
    VmaAllocation default_shadow_allocation = nullptr;
    VkImageView default_shadow_view = VK_NULL_HANDLE;
    VkSampler default_shadow_sampler = VK_NULL_HANDLE;

    // Window/viewport state
    WindowHandle window_handle = nullptr;
    int viewport_width = 0;
    int viewport_height = 0;
    float field_of_view = 75.0f;

    // Current render state
    RenderState current_state;
    bool frame_started = false;
    bool image_acquired = false;
    bool device_lost = false;

    // Clear color (set by clear(), used in beginFrame)
    glm::vec3 clear_color = glm::vec3(0.2f, 0.3f, 0.8f);

    // CSM shadow mapping
    static const int NUM_CASCADES = 4;
    uint32_t currentShadowSize = 4096;  // Runtime configurable shadow resolution
    int shadowQuality = 3;  // 0=Off, 1=Low(1024), 2=Medium(2048), 3=High(4096)
    int pendingShadowQuality = -1;  // Deferred quality change (-1 = none pending)
    int activeCascadeCount = 2;
    bool fxaaEnabled = true;
    bool debugCascades = false;
    float cascadeSplitDistances[5] = { 0.1f, 10.0f, 35.0f, 90.0f, 200.0f };
    float cascadeSplitLambda = 0.92f;
    glm::mat4 lightSpaceMatrices[4] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };

    // Shadow map resources
    VkImage shadow_map_image = VK_NULL_HANDLE;
    VmaAllocation shadow_map_allocation = nullptr;
    VkImageView shadow_map_view = VK_NULL_HANDLE;          // Full array view for sampling
    VkImageView shadow_cascade_views[4] = {};               // Per-cascade views for framebuffer
    VkSampler shadow_sampler = VK_NULL_HANDLE;
    VkFramebuffer shadow_framebuffers[4] = {};
    VkRenderPass shadow_render_pass = VK_NULL_HANDLE;

    // Shadow pipeline
    VkPipeline shadow_pipeline = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_alpha_test = VK_NULL_HANDLE;
    VkPipelineLayout shadow_pipeline_layout = VK_NULL_HANDLE;
    VkPipelineLayout shadow_alphatest_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadow_descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool shadow_descriptor_pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> shadow_descriptor_sets;
    std::vector<VkBuffer> shadow_uniform_buffers;
    std::vector<VmaAllocation> shadow_uniform_allocations;
    std::vector<void*> shadow_uniform_mapped;

    // Shadow pass state
    bool in_shadow_pass = false;
    int currentCascade = 0;
    bool main_pass_started = false;
    bool shadow_pass_active = false;

    // Skybox resources
    VkPipeline skybox_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout skybox_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout skybox_descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool skybox_descriptor_pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> skybox_descriptor_sets;
    std::vector<VkBuffer> skybox_uniform_buffers;
    std::vector<VmaAllocation> skybox_uniform_allocations;
    std::vector<void*> skybox_uniform_mapped;
    VkSampler skybox_depth_sampler = VK_NULL_HANDLE;
    bool skybox_initialized = false;

    // Render graph skybox resources (color loadOp=LOAD + depth read-only)
    VkRenderPass  skybox_rg_render_pass  = VK_NULL_HANDLE;
    VkFramebuffer skybox_rg_framebuffer  = VK_NULL_HANDLE;
    bool          m_skyboxRequested      = false;

    // Post-processing resources
    VkImage offscreen_image = VK_NULL_HANDLE;
    VmaAllocation offscreen_allocation = nullptr;
    VkImageView offscreen_view = VK_NULL_HANDLE;
    VkSampler offscreen_sampler = VK_NULL_HANDLE;
    VkRenderPass offscreen_render_pass = VK_NULL_HANDLE;
    VkRenderPass transparent_forward_render_pass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> offscreen_framebuffers;
    VkImage offscreen_depth_image = VK_NULL_HANDLE;
    VmaAllocation offscreen_depth_allocation = nullptr;
    VkImageView offscreen_depth_view = VK_NULL_HANDLE;

    VkBuffer fxaa_vertex_buffer = VK_NULL_HANDLE;
    VmaAllocation fxaa_vertex_allocation = nullptr;
    VulkanPostProcessPass fxaaPass_;                   // FXAA pipeline, render pass, descriptors, UBOs
    std::vector<VkFramebuffer> fxaa_framebuffers;      // Swapchain framebuffers (shared with renderUI)
    VkPipeline viewport_fxaa_pipeline = VK_NULL_HANDLE;
    bool fxaa_initialized = false;

    // Deferred GBuffer geometry pass (PSO + 3-RT render pass owner)
    VulkanGBufferPass gbufferPass_;
    bool gbuffer_initialized = false;
    VulkanDeferredLightingPass deferredLightingPass_;
    bool deferred_lighting_initialized = false;
    bool createGBufferResources();
    void cleanupGBufferResources();
    bool createDeferredLightingResources();
    void cleanupDeferredLightingResources();

    // SSAO post-process passes
    VulkanPostProcessPass ssaoPass_;        // computation
    VulkanPostProcessPass ssaoBlurHPass_;   // horizontal blur
    VulkanPostProcessPass ssaoBlurVPass_;   // vertical blur

    // Manual SSAO resources (not managed by VulkanPostProcessPass)
    VkImage ssao_noise_image = VK_NULL_HANDLE;
    VmaAllocation ssao_noise_allocation = nullptr;
    VkImageView ssao_noise_view = VK_NULL_HANDLE;
    VkImageView ssao_depth_view = VK_NULL_HANDLE;
    VkSampler ssao_depth_sampler = VK_NULL_HANDLE;
    VkSampler ssao_noise_sampler = VK_NULL_HANDLE;
    VkSampler ssao_linear_sampler = VK_NULL_HANDLE;
    VkImage ssao_fallback_image = VK_NULL_HANDLE;
    VmaAllocation ssao_fallback_allocation = nullptr;
    VkImageView ssao_fallback_view = VK_NULL_HANDLE;
    bool ssao_initialized = false;
    bool ssaoEnabled = true;
    float ssaoRadius = 0.5f;
    float ssaoIntensity = 1.5f;
    float ssaoBias = 0.025f;
    glm::vec4 ssaoKernel[16];

    bool createSSAOResources();
    void cleanupSSAOResources();
    void recreateSSAOResources();
    void generateSSAOKernel();

    // Shadow mask post-process pass
    VulkanPostProcessPass shadowMaskPass_;
    VkImageView shadow_mask_depth_view = VK_NULL_HANDLE;
    VkSampler shadow_mask_depth_sampler = VK_NULL_HANDLE;
    bool shadow_mask_initialized = false;

    bool createShadowMaskResources();
    void cleanupShadowMaskResources();
    void recreateShadowMaskResources();

    // Render graph
    RenderGraph m_frameGraph;
    VulkanRGBackend m_rgBackend;
    VulkanPostProcessGraphBuilder m_ppGraphBuilder;
    bool m_useRenderGraph = true;
    bool m_useDeferred = false;

    // Deferred-path command buffering (mirrors D3D12 layout). The renderer
    // routes opaque/transparent draws here when isDeferredActive() returns
    // true; the GBuffer / TransparentForward graph passes replay them.
    RenderCommandBuffer m_deferredOpaqueCmds;
    RenderCommandBuffer m_deferredTransparentCmds;
    std::vector<vertex> m_deferredDebugLineVertices;

    // Optional pipeline override for replay paths. When non-null, the buffered
    // command replay loops bind this pipeline instead of the per-cmd selection.
    // Set by the GBuffer pass (to bind gbufferPass_.getPipeline()) and cleared
    // afterwards. Mirrors D3D12RenderAPI::m_replayPSOOverride.
    VkPipeline m_replayPipelineOverride = VK_NULL_HANDLE;

    // Viewport render target for editor
    VkImage viewport_image = VK_NULL_HANDLE;
    VmaAllocation viewport_allocation = VK_NULL_HANDLE;
    VkImageView viewport_view = VK_NULL_HANDLE;
    VkSampler viewport_sampler = VK_NULL_HANDLE;
    VkDescriptorSet viewport_imgui_ds = VK_NULL_HANDLE;
    VkImage viewport_depth_image = VK_NULL_HANDLE;
    VmaAllocation viewport_depth_allocation = nullptr;
    VkImageView viewport_depth_view = VK_NULL_HANDLE;
    VkRenderPass viewport_resolve_pass = VK_NULL_HANDLE;
    VkFramebuffer viewport_framebuffer = VK_NULL_HANDLE;
    VkRenderPass ui_render_pass = VK_NULL_HANDLE;
    int viewport_width_rt = 0;
    int viewport_height_rt = 0;
    void createViewportResources(int w, int h);
    void destroyViewportResources();
    // True when the API is rendering for an editor (legacy viewport_image set
    // up, OR the new SceneViewport wrapper bound via setEditorViewport). Used
    // throughout the Vulkan path to gate "I'm an editor render" branches.
    bool isViewportMode() const
    {
        return viewport_image != VK_NULL_HANDLE || m_editor_scene_viewport != nullptr;
    }

    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;

public:
    // Viewport rendering (for editor)
    virtual void endSceneRender() override;
    virtual uint64_t getViewportTextureID() override;
    virtual void setViewportSize(int width, int height) override;
    virtual void renderUI() override;

    // SceneViewport-based editor path. Each call to createSceneViewport
    // allocates a fresh PIE viewport under the hood (the wrapper owns it
    // and frees on destruction). setEditorViewport routes the active scene
    // target to whichever wrapper is bound.
    std::unique_ptr<SceneViewport> createSceneViewport(int width, int height) override;
    void setEditorViewport(SceneViewport* viewport) override;

private:
    // Non-owning pointer to the editor's currently-bound scene viewport. Set
    // by setEditorViewport. Used by setViewportSize / getViewportTextureID
    // to forward to the active wrapper instead of the legacy viewport_image.
    VulkanSceneViewport* m_editor_scene_viewport = nullptr;
public:

    // Preview render target (for asset preview panel)
    virtual void beginPreviewFrame(int width, int height) override;
    virtual void endPreviewFrame() override;
    virtual uint64_t getPreviewTextureID() override;
    virtual void destroyPreviewTarget() override;

private:
    // Preview render target resources
    VkImage preview_image = VK_NULL_HANDLE;
    VmaAllocation preview_allocation = VK_NULL_HANDLE;
    VkImageView preview_view = VK_NULL_HANDLE;
    VkSampler preview_sampler = VK_NULL_HANDLE;
    VkDescriptorSet preview_imgui_ds = VK_NULL_HANDLE;
    VkImage preview_depth_image = VK_NULL_HANDLE;
    VmaAllocation preview_depth_allocation = nullptr;
    VkImageView preview_depth_view = VK_NULL_HANDLE;
    VkFramebuffer preview_framebuffer = VK_NULL_HANDLE;
    int preview_width_rt = 0, preview_height_rt = 0;
    void createPreviewResources(int w, int h);
    void destroyPreviewResources();

public:
    // PIE (Play-In-Editor) viewport render targets
    virtual int  createPIEViewport(int width, int height) override;
    virtual void destroyPIEViewport(int id) override;
    virtual void destroyAllPIEViewports() override;
    virtual void setPIEViewportSize(int id, int width, int height) override;
    virtual void setActiveSceneTarget(int pie_viewport_id) override;
    virtual uint64_t getPIEViewportTextureID(int id) override;

private:
    struct PIEViewportTarget {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSet imgui_ds = VK_NULL_HANDLE;
        VkImage hdr_image = VK_NULL_HANDLE;
        VmaAllocation hdr_allocation = nullptr;
        VkImageView hdr_view = VK_NULL_HANDLE;
        VkImage depth_image = VK_NULL_HANDLE;
        VmaAllocation depth_allocation = nullptr;
        VkImageView depth_view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;          // offscreen pass framebuffer (color+depth)
        VkFramebuffer resolve_framebuffer = VK_NULL_HANDLE;  // resolve pass framebuffer (color only, for FXAA)
        int width = 0, height = 0;
    };
    std::unordered_map<int, PIEViewportTarget> m_pie_viewports;
    int m_next_pie_id = 0;
    int m_active_scene_target = -1;

    void createPIEViewportResources(PIEViewportTarget& target, int w, int h);
    void destroyPIEViewportResources(PIEViewportTarget& target);
};
