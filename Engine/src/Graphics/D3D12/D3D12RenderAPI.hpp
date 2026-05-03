#pragma once

#include "Graphics/RenderAPI.hpp"
#include "Graphics/RenderCommandBuffer.hpp"
#include "D3D12Types.hpp"
#include "D3D12BarrierBatch.hpp"
#include "D3D12CommandListPool.hpp"
#include "D3D12CopyQueue.hpp"
#include "D3D12PSOCache.hpp"
#include "D3D12PostProcessPass.hpp"
#include <functional>
#include "D3D12GBufferPass.hpp"
#include "D3D12ResourceStateTracker.hpp"
#include "D3D12RGBackend.hpp"
#include "D3D12PostProcessGraphBuilder.hpp"
#include "D3D12DeferredSceneGraphBuilder.hpp"
#include "Graphics/RenderGraph/RenderGraph.hpp"
#include "D3D12SceneViewport.hpp"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <dxgidebug.h>
#include <wrl/client.h>
#include <SDL3/SDL.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <stack>
#include <array>
#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>
#include <memory>


using Microsoft::WRL::ComPtr;

// Forward declarations
class D3D12Mesh;

class D3D12RenderAPI : public IRenderAPI
{
public:
    static const int NUM_BACK_BUFFERS = 2;
    static const int NUM_FRAMES_IN_FLIGHT = 2;

private:
    friend class D3D12PostProcessGraphBuilder;
    friend class D3D12DeferredSceneGraphBuilder;
    friend class D3D12SceneViewport;
    friend class D3D12PostProcessPass;

    WindowHandle window_handle = nullptr;
    HWND hwnd = nullptr;
    int viewport_width = 0;
    int viewport_height = 0;
    float field_of_view = 75.0f;
    RenderState current_state;

    // Core D3D12 objects
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<IDXGIFactory4> dxgiFactory;

    // Per-frame resources
    struct FrameContext
    {
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        UINT64 fenceValue = 0;
    };
    FrameContext m_frameContexts[NUM_FRAMES_IN_FLIGHT];
    UINT m_frameIndex = 0;
    UINT m_backBufferIndex = 0;

    // Command list (single, reused each frame)
    ComPtr<ID3D12GraphicsCommandList> commandList;

    // Fence for CPU/GPU synchronization
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;

    // Back buffer resources
    ComPtr<ID3D12Resource> m_backBuffers[NUM_BACK_BUFFERS];

    // Client-mode scene viewport. Owns the HDR + depth that the standalone
    // render path draws into, and each frame rebinds the current swap-chain
    // back buffer as its LDR output. Null in editor mode.
    std::unique_ptr<D3D12SceneViewport> m_clientViewport;

    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap; // Shader-visible CBV_SRV_UAV

    DescriptorHeapAllocator m_rtvAllocator;
    DescriptorHeapAllocator m_dsvAllocator;
    DescriptorHeapAllocator m_srvAllocator;

    // RTV indices for back buffers
    UINT m_backBufferRTVs[NUM_BACK_BUFFERS] = { UINT(-1), UINT(-1) };

    // Root signature
    ComPtr<ID3D12RootSignature> m_rootSignature;

    // PSO cache
    D3D12PSOCache m_psoCache;
    std::string m_psoCachePath;

    // Pipeline State Objects
    ComPtr<ID3D12PipelineState> m_psoBasicLit;
    ComPtr<ID3D12PipelineState> m_psoBasicLitCullFront;
    ComPtr<ID3D12PipelineState> m_psoBasicLitCullNone;
    ComPtr<ID3D12PipelineState> m_psoBasicLitAlpha;
    ComPtr<ID3D12PipelineState> m_psoBasicLitAlphaCullNone;
    ComPtr<ID3D12PipelineState> m_psoBasicLitAdditive;
    ComPtr<ID3D12PipelineState> m_psoUnlit;
    ComPtr<ID3D12PipelineState> m_psoUnlitCullNone;
    ComPtr<ID3D12PipelineState> m_psoUnlitAlpha;
    ComPtr<ID3D12PipelineState> m_psoUnlitAlphaCullNone;
    ComPtr<ID3D12PipelineState> m_psoUnlitAdditive;
    ComPtr<ID3D12PipelineState> m_psoShadow;
    ComPtr<ID3D12PipelineState> m_psoShadowAlphaTest;
    ComPtr<ID3D12PipelineState> m_psoDepthPrepass;
    ComPtr<ID3D12PipelineState> m_psoDepthPrepassAlphaTest;
    ComPtr<ID3D12PipelineState> m_psoDepthPrepassAlphaTestCullNone;
    ComPtr<ID3D12PipelineState> m_psoDebugLines;

    // Shader bytecode (DXIL)
    std::vector<char> m_basicVS, m_basicPS;
    std::vector<char> m_unlitVS, m_unlitPS;
    std::vector<char> m_shadowVS, m_shadowPS;
    std::vector<char> m_shadowAlphaTestVS, m_shadowAlphaTestPS;
    std::vector<char> m_skyVS, m_skyPS;
    std::vector<char> m_fxaaVS, m_fxaaPS;
    std::vector<char> m_gbufferVS, m_gbufferPS;
    std::vector<char> m_deferredLightingVS, m_deferredLightingPS;

    // Per-frame upload ring buffers for constant data
    UploadRingBuffer m_cbUploadBuffer[NUM_FRAMES_IN_FLIGHT];

    // Async copy queue for texture uploads
    D3D12CopyQueue m_copyQueue;

    // Command list pool for parallel replay (multicore rendering)
    D3D12CommandListPool m_commandListPool;

    // Upload command infrastructure (for meshes and init-time buffer creation)
    ComPtr<ID3D12CommandAllocator> m_uploadCmdAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_uploadCmdList;
    ComPtr<ID3D12Fence> m_uploadFence;
    HANDLE m_uploadFenceEvent = nullptr;
    UINT64 m_uploadFenceValue = 0;

    // Matrix management
    glm::mat4 projection_matrix = glm::mat4(1.0f);
    glm::mat4 view_matrix = glm::mat4(1.0f);
    glm::mat4 current_model_matrix = glm::mat4(1.0f);
    std::stack<glm::mat4> model_matrix_stack;

    // Lighting state
    glm::vec3 current_light_direction = glm::vec3(0.0f, -1.0f, 0.0f);
    glm::vec3 current_light_ambient = glm::vec3(0.2f, 0.2f, 0.2f);
    glm::vec3 current_light_diffuse = glm::vec3(0.8f, 0.8f, 0.8f);
    bool lighting_enabled = true;
    LightCBuffer current_lights{};

    // Shadow Mapping - CSM
    static const int NUM_CASCADES = 4;
    unsigned int currentShadowSize = 4096;
    int shadowQuality = 3;
    ComPtr<ID3D12Resource> m_shadowMapArray;
    UINT m_shadowDSVIndices[NUM_CASCADES] = { UINT(-1), UINT(-1), UINT(-1), UINT(-1) };
    UINT m_shadowSRVIndex = UINT(-1);
    ComPtr<ID3D12Resource> m_dummyShadowTexture;  // 1x1 Texture2DArray placeholder for shadow map slot
    UINT m_dummyShadowSRVIndex = UINT(-1);
    glm::mat4 lightSpaceMatrix = glm::mat4(1.0f);
    glm::mat4 lightSpaceMatrices[NUM_CASCADES];
    float cascadeSplitDistances[NUM_CASCADES + 1] = {};
    float cascadeSplitLambda = 0.92f;
    int currentCascade = 0;
    bool in_shadow_pass = false;
    bool debugCascades = false;

    // Deferred GBuffer geometry pass
    D3D12GBufferPass m_gbufferPass;
    // Deferred lighting fullscreen pass (reads GBuffer, writes HDR).
    D3D12PostProcessPass m_deferredLightingPass;

    // Post-processing (FXAA)
    D3D12PostProcessPass m_fxaaPass;
    ComPtr<ID3D12Resource> m_fxaaQuadVB;
    D3D12_VERTEX_BUFFER_VIEW m_fxaaQuadVBV = {};
    bool fxaaEnabled = true;
    void renderFXAAPass(D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, UINT inputSRVIndex,
                        int width, int height, bool enableSSAO, bool enableShadowMask);

    // SSAO
    D3D12PostProcessPass m_ssaoPass;
    D3D12PostProcessPass m_ssaoBlurHPass;
    D3D12PostProcessPass m_ssaoBlurVPass;
    ComPtr<ID3D12Resource> m_ssaoNoiseTexture;
    ComPtr<ID3D12Resource> m_ssaoFallbackTexture;   // 1x1 white
    UINT m_ssaoNoiseSRVIndex = UINT(-1);
    UINT m_ssaoFallbackSRVIndex = UINT(-1);
    bool ssaoEnabled = true;
    float ssaoRadius = 0.5f;
    float ssaoIntensity = 1.5f;
    float ssaoBias = 0.025f;
    glm::vec4 ssaoKernel[16];  // Pre-generated hemisphere samples

    // Shadow Mask Post-Process
    D3D12PostProcessPass m_shadowMaskPass;

    // Skybox (post-process pass)
    D3D12PostProcessPass m_skyPass;

    // Texture management
    std::unordered_map<TextureHandle, D3D12Texture> textures;
    TextureHandle nextTextureHandle = 1;
    TextureHandle currentBoundTexture = INVALID_TEXTURE;
    TextureHandle defaultTexture = INVALID_TEXTURE;

    // Default PBR textures (1x1 placeholders)
    D3D12Texture m_defaultNormalTexture;             // RGBA(128, 128, 255, 255) = flat normal
    D3D12Texture m_defaultMetallicRoughnessTexture;  // RGBA(0, 128, 0, 255) = metallic=0, roughness=0.5
    D3D12Texture m_defaultOcclusionTexture;          // RGBA(255, 255, 255, 255) = no occlusion
    D3D12Texture m_defaultEmissiveTexture;           // RGBA(0, 0, 0, 255) = no emission

    // State tracking
    ID3D12PipelineState* last_bound_pso = nullptr;
    bool global_cbuffer_dirty = true;
    bool in_depth_prepass = false;
    D3D12_GPU_VIRTUAL_ADDRESS m_cachedLightCBAddr = 0; // Uploaded once per frame, reused per mesh
    D3D12_GPU_VIRTUAL_ADDRESS m_dummyCBAddr = 0;       // Dummy CB for bindDummyRootParams, allocated once per frame

    // Command list lifecycle
    bool m_commandListOpen = false;
    void ensureCommandListOpen();

    // Current render target state (cached for parallel command list setup)
    struct FrameRenderTargetState
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
        D3D12_VIEWPORT viewport = {};
        D3D12_RECT scissor = {};
        bool valid = false;
    };
    FrameRenderTargetState m_currentRT;

    // Flush the main command list (close + submit), then reopen it for further work.
    // Used to interleave parallel command list submissions.
    void flushAndReopenCommandList();

    // Automatic resource state tracking
    D3D12ResourceStateTracker m_stateTracker;

    // Deferred-release ring. External owners (meshes, textures, viewport
    // targets, etc.) hand their ComPtrs here when they'd otherwise Release
    // immediately. A slot is only cleared after NUM_FRAMES_IN_FLIGHT + 1
    // advances, by which point the GPU has cycled past every command list
    // that could have referenced the parked resource. Mutex-guarded because
    // meshes can be destroyed from worker threads (asset streaming).
    static constexpr int kDeferredReleaseSlots = NUM_FRAMES_IN_FLIGHT + 1;
    std::vector<ComPtr<IUnknown>> m_deferredRelease[kDeferredReleaseSlots];
    // Parallel ring for non-IUnknown cleanups (e.g. descriptor-heap slot frees).
    // Runs after kDeferredReleaseSlots frames so the GPU is past any reference.
    std::vector<std::function<void()>> m_deferredDescriptorFrees[kDeferredReleaseSlots];
    int m_deferredReleaseSlot = 0;
    std::mutex m_deferredReleaseMutex;
    void enqueueDeferredRelease(ComPtr<IUnknown> resource);
    void enqueueDeferredFree(std::function<void()> cleanup);
    void flushDeferredReleases();   // advance the ring; called in ensureCommandListOpen after the fence-wait

    // Device lost flag
    bool device_lost = false;

    // Deferred shadow map recreation
    bool shadow_resources_dirty = false;
    unsigned int pending_shadow_size = 0;

    // Deferred SSAO / shadow-mask viewport-size change. setViewportSize can be
    // called mid-frame (editor: main -> PIE -> main); destroying these textures
    // immediately would invalidate references already recorded in the open
    // command list. We stash the requested size and apply it inside
    // ensureCommandListOpen on the NEXT frame, after the fence has retired
    // any work that referenced the old textures.
    bool pp_resize_dirty = false;
    int  pp_resize_width = 0;
    int  pp_resize_height = 0;

    // VSync / present interval
    int presentInterval = 1;

    // Internal helper methods
    bool createDevice();
    bool createCommandQueue();
    bool createSwapChain();
    bool createDescriptorHeaps();
    bool createBackBufferRTVs();
    bool createFrameResources();
    bool createFence();
    bool createUploadInfrastructure();
    bool createRootSignature();
    bool loadShaders();
    bool createPipelineStates();
    bool createConstantBufferUploadHeaps();
    bool createShadowMapResources();
    bool createPostProcessSharedResources();   // FXAA quad VB + SSAO fallback texture
    bool createSSAOResources(int width, int height);
    void renderSSAOPass(ID3D12Resource* depthBuffer, UINT depthSRVIndex, int fullWidth, int fullHeight);
    void generateSSAOKernel();
    bool createShadowMaskResources(int width, int height);
    void renderShadowMaskPass(ID3D12Resource* depthBuffer, UINT depthSRVIndex, int fullWidth, int fullHeight);
    bool createSkyboxPass(int width, int height);
    bool createDefaultTexture();
    bool createDefaultPBRTextures();

    // Render graph
    RenderGraph m_frameGraph;
    D3D12RGBackend m_rgBackend;
    D3D12PostProcessGraphBuilder m_ppGraphBuilder;
    D3D12DeferredSceneGraphBuilder m_deferredGraphBuilder;
    bool m_useRenderGraph = true;
    bool m_useDeferred = false;
    bool m_skyboxRequested = false;

    // Buffered opaque/transparent commands for the deferred path.
    RenderCommandBuffer m_deferredOpaqueCmds;
    RenderCommandBuffer m_deferredTransparentCmds;
    // Buffered debug-line vertices for deferred replay. Forward draws them
    // straight to HDR during the scene phase; in deferred we defer them into
    // the TransparentForward RG pass so the lighting pass doesn't overwrite
    // them with (albedo=0, emissive=0) pixels.
    std::vector<vertex> m_deferredDebugLineVertices;
    // Optional PSO override used by replayCommandBuffer main-pass loop. Set while
    // replaying opaques into the GBuffer pass so the GBuffer PSO is bound instead
    // of the forward-lit PSO.
    ID3D12PipelineState* m_replayPSOOverride = nullptr;

    // Deferred point/spot light StructuredBuffers. Per-frame to avoid GPU hazard
    // while the CPU writes the next frame's data. Persistently mapped.
    static const int MAX_LIGHTS_DEFERRED = 256;
    ComPtr<ID3D12Resource> m_pointLightsSB[NUM_FRAMES_IN_FLIGHT];
    ComPtr<ID3D12Resource> m_spotLightsSB[NUM_FRAMES_IN_FLIGHT];
    UINT m_pointLightsSRVIndex[NUM_FRAMES_IN_FLIGHT] = { UINT(-1), UINT(-1) };
    UINT m_spotLightsSRVIndex[NUM_FRAMES_IN_FLIGHT]  = { UINT(-1), UINT(-1) };
    void* m_pointLightsSBMapped[NUM_FRAMES_IN_FLIGHT] = { nullptr, nullptr };
    void* m_spotLightsSBMapped[NUM_FRAMES_IN_FLIGHT]  = { nullptr, nullptr };
    int   m_numPointLights = 0;
    int   m_numSpotLights  = 0;
    bool  createDeferredLightBuffers();
    bool createDummyShadowTexture();

    void waitForFence(UINT64 fenceValue);
    void flushGPU();
    void executeUploadCommandList();

    std::vector<char> readShaderBinary(const std::string& filepath);

    void transitionResource(ID3D12Resource* resource,
                            D3D12_RESOURCE_STATES before,
                            D3D12_RESOURCE_STATES after);
    void flushBarriers();
    BarrierBatch m_barrierBatch;

    void bindDummyRootParams();

    ID3D12PipelineState* selectPSO(const RenderState& state, bool unlit);
    void updateGlobalCBuffer();
    void updatePerObjectCBuffer(const glm::vec3& color, bool useTexture, float alphaCutoff = 0.0f);
    void updateShadowCBuffer(const glm::mat4& lightSpace, const glm::mat4& model);

    // CSM helper methods
    void calculateCascadeSplits(float nearPlane, float farPlane);
    std::array<glm::vec3, 8> getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view);
    glm::mat4 getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
                                             const glm::mat4& viewMatrix, float fov, float aspect);
    void recreateShadowMapResources(unsigned int size);

    // Helper to create a GPU buffer from CPU data
    ComPtr<ID3D12Resource> createBufferFromData(const void* data, size_t dataSize,
                                                 D3D12_RESOURCE_STATES finalState);

    // CPU-side mipmap generation
    std::vector<uint8_t> generateMipLevel(const uint8_t* src, int srcWidth, int srcHeight, int channels,
                                           int& outWidth, int& outHeight);

public:
    D3D12RenderAPI();
    virtual ~D3D12RenderAPI();

    // IRenderAPI implementation
    bool initialize(WindowHandle window, int width, int height, float fov) override;
    void shutdown() override;
    void waitForGPU() override;
    void resize(int width, int height) override;

    void beginFrame() override;
    void endFrame() override;
    void present() override;
    void clear(const glm::vec3& color = glm::vec3(0.2f, 0.3f, 0.8f)) override;

    void setCamera(const camera& cam) override;
    void pushMatrix() override;
    void popMatrix() override;
    void translate(const glm::vec3& pos) override;
    void rotate(const glm::mat4& rotation) override;
    void multiplyMatrix(const glm::mat4& matrix) override;

    glm::mat4 getProjectionMatrix() const override;
    glm::mat4 getViewMatrix() const override;

    TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) override;
    TextureHandle loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                        bool flip_vertically = false, bool generate_mipmaps = true) override;
    TextureHandle loadCompressedTexture(int width, int height, uint32_t format, int mip_count,
                                        const std::vector<const uint8_t*>& mip_data,
                                        const std::vector<size_t>& mip_sizes,
                                        const std::vector<std::pair<int,int>>& mip_dimensions) override;
    void bindTexture(TextureHandle texture) override;
    void unbindTexture() override;
    void deleteTexture(TextureHandle texture) override;

    void renderMesh(const mesh& m, const RenderState& state = RenderState()) override;
    void renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state = RenderState()) override;

    void setRenderState(const RenderState& state) override;
    void enableLighting(bool enable) override;
    void setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction) override;
    void setPointAndSpotLights(const LightCBuffer& lights) override;

    void renderSkybox() override;

    // Shadow Mapping (CSM)
    void beginShadowPass(const glm::vec3& lightDir) override;
    void beginShadowPass(const glm::vec3& lightDir, const camera& cam) override;
    void beginCascade(int cascadeIndex) override;
    void endShadowPass() override;
    void bindShadowMap(int textureUnit) override;
    glm::mat4 getLightSpaceMatrix() override;
    int getCascadeCount() const override;
    const float* getCascadeSplitDistances() const override;
    const glm::mat4* getLightSpaceMatrices() const override;

    // Depth prepass
    void beginDepthPrepass() override;
    void endDepthPrepass() override;
    void renderMeshDepthOnly(const mesh& m) override;

    IGPUMesh* createMesh() override;

    // Command buffer replay (multicore rendering)
    void replayCommandBuffer(const RenderCommandBuffer& cmds) override;

    // Parallel replay: splits commands across multiple command lists recorded on worker threads.
    // Falls back to single-threaded replay for small command counts.
    void replayCommandBufferParallel(const RenderCommandBuffer& cmds) override;

    void setDeferredEnabled(bool enabled) override;
    bool isDeferredEnabled() const override { return m_useDeferred; }
    bool isDeferredActive() const override;
    void submitDeferredOpaqueCommands(const RenderCommandBuffer& cmds) override;
    void submitDeferredTransparentCommands(const RenderCommandBuffer& cmds) override;
    void uploadLightBuffers(const GPUPointLight* pts, int ptCount,
                            const GPUSpotLight* spts, int spCount) override;

    // Debug line rendering
    void renderDebugLines(const vertex* vertices, size_t vertex_count) override;
    // Unbuffered variant: draw debug lines directly onto whatever RT is currently
    // bound. Used by the deferred RG pass to replay buffered vertices after
    // lighting/skybox/transparent. Normal callers should use renderDebugLines,
    // which routes through the deferred buffer when deferred is active.
    void renderDebugLinesDirect(const vertex* vertices, size_t vertex_count);

    const char* getAPIName() const override { return "D3D12"; }

    // Graphics settings
    void setFXAAEnabled(bool enabled) override;
    bool isFXAAEnabled() const override;
    void setShadowQuality(int quality) override;
    int getShadowQuality() const override;
    void setSSAOEnabled(bool enabled) override;
    bool isSSAOEnabled() const override;
    void setSSAORadius(float radius) override;
    void setSSAOIntensity(float intensity) override;

    // Viewport rendering (for editor)
    void endSceneRender() override;
    uint64_t getViewportTextureID() override;
    void setViewportSize(int width, int height) override;
    void renderUI() override;

    // New SceneViewport-based editor path. Caller owns the viewport.
    std::unique_ptr<SceneViewport> createSceneViewport(int width, int height) override;
    void setEditorViewport(SceneViewport* viewport) override;

    // Preview render target (for asset preview panel)
    void beginPreviewFrame(int width, int height) override;
    void endPreviewFrame() override;
    uint64_t getPreviewTextureID() override;
    void destroyPreviewTarget() override;

    // PIE viewport render targets
    int  createPIEViewport(int width, int height) override;
    void destroyPIEViewport(int id) override;
    void destroyAllPIEViewports() override;
    void setPIEViewportSize(int id, int width, int height) override;
    void setActiveSceneTarget(int pie_viewport_id) override;
    uint64_t getPIEViewportTextureID(int id) override;

    // D3D12 specific accessors (for ImGui integration)
    ID3D12Device* getDevice() const { return device.Get(); }
    ID3D12CommandQueue* getCommandQueue() const { return commandQueue.Get(); }
    ID3D12GraphicsCommandList* getCommandList() const { return commandList.Get(); }
    ID3D12DescriptorHeap* getSrvDescriptorHeap() const { return m_srvHeap.Get(); }
    DescriptorHeapAllocator& getSrvAllocator() { return m_srvAllocator; }
    void deferRTVFree(UINT index);
    void deferDSVFree(UINT index);
    void deferSRVFree(UINT index);

    // Hand a D3D12 resource to the deferred-release ring. The caller's ComPtr
    // is Reset to null; the underlying object is kept alive for
    // NUM_FRAMES_IN_FLIGHT + 1 frames before its refcount drops. Use this
    // instead of letting a ComPtr Release during a frame — any resource still
    // referenced by the currently-open command list would otherwise trigger
    // OBJECT_DELETED_WHILE_STILL_IN_USE at Close().
    template <typename T>
    void deferredRelease(ComPtr<T>& resource)
    {
        if (!resource) return;
        ComPtr<IUnknown> u = resource;  // AddRef via T* -> IUnknown* conversion
        enqueueDeferredRelease(std::move(u));
        resource.Reset();
    }

private:
    // Editor main-viewport render target. null in standalone-client mode OR
    // when the editor hasn't yet registered its viewport.
    // Ownership lives in the caller (EditorApp). The API just stores a
    // non-owning pointer that's set via setEditorViewport().
    D3D12SceneViewport* m_editorViewport = nullptr;
    // Cached size of the current scene-render target (editor viewport when
    // editor mode, PIE viewport during PIE render, window size otherwise).
    // Used to size the API-wide SSAO / shadow-mask buffers.
    int viewport_width_rt = 0, viewport_height_rt = 0;

    // Preview render target for asset preview panel
    ComPtr<ID3D12Resource> m_previewTexture;
    ComPtr<ID3D12Resource> m_previewDepthBuffer;
    UINT m_previewRTVIndex = UINT(-1);
    UINT m_previewSRVIndex = UINT(-1);
    UINT m_previewDSVIndex = UINT(-1);
    int preview_width_rt = 0, preview_height_rt = 0;
    void createPreviewResources(int w, int h);

    // PIE viewport render targets. Each PIE client owns one SceneViewport;
    // the viewport's destructor routes resources through the deferred-release
    // ring and cleans up descriptors.
    std::unordered_map<int, std::unique_ptr<D3D12SceneViewport>> m_pie_viewports;
    int m_next_pie_id = 0;
    int m_active_scene_target = -1;
};
