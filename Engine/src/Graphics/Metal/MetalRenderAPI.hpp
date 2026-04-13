#pragma once

#include "Graphics/RenderAPI.hpp"
#include <memory>

// Forward declaration - implementation is in MetalRenderAPI*.mm (Objective-C++)
struct MetalRenderAPIImpl;

class MetalRenderAPI : public IRenderAPI
{
public:
    MetalRenderAPI();
    virtual ~MetalRenderAPI();

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

    // Command buffer replay (multicore rendering)
    virtual void replayCommandBuffer(const RenderCommandBuffer& cmds) override;
    virtual void replayCommandBufferParallel(const RenderCommandBuffer& cmds) override;

    // Debug rendering
    virtual void renderDebugLines(const vertex* vertices, size_t vertex_count) override;

    // Depth prepass
    virtual void beginDepthPrepass() override;
    virtual void endDepthPrepass() override;
    virtual void renderMeshDepthOnly(const mesh& m) override;

    // GPU sync
    virtual void waitForGPU() override;

    // Shadow Mapping (CSM)
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

    virtual const char* getAPIName() const override { return "Metal"; }

    // Graphics settings
    virtual void setFXAAEnabled(bool enabled) override;
    virtual bool isFXAAEnabled() const override;
    virtual void setShadowQuality(int quality) override;
    virtual int getShadowQuality() const override;
    virtual void setSSAOEnabled(bool enabled) override;
    virtual bool isSSAOEnabled() const override;
    virtual void setSSAORadius(float radius) override;
    virtual void setSSAOIntensity(float intensity) override;

    // Autorelease pool support (drains ObjC temporaries each frame)
    virtual void executeWithAutoreleasePool(std::function<void()> fn) override;

    // Viewport rendering (for editor)
    virtual void endSceneRender() override;
    virtual uint64_t getViewportTextureID() override;
    virtual void setViewportSize(int width, int height) override;
    virtual void renderUI() override;

    // Preview render target (for asset preview panel)
    virtual void beginPreviewFrame(int width, int height) override;
    virtual void endPreviewFrame() override;
    virtual uint64_t getPreviewTextureID() override;
    virtual void destroyPreviewTarget() override;

    // PIE viewport render targets (for multi-player Play-In-Editor)
    virtual int  createPIEViewport(int width, int height) override;
    virtual void destroyPIEViewport(int id) override;
    virtual void destroyAllPIEViewports() override;
    virtual void setPIEViewportSize(int id, int width, int height) override;
    virtual void setActiveSceneTarget(int pie_viewport_id) override;
    virtual uint64_t getPIEViewportTextureID(int id) override;

    // Metal-specific accessors for ImGui integration
    void* getDevice() const;           // Returns id<MTLDevice> as void*
    void* getCommandBuffer() const;    // Returns id<MTLCommandBuffer> as void*
    void* getRenderPassDescriptor() const; // Returns MTLRenderPassDescriptor* as void*
    void* getRenderCommandEncoder() const; // Returns id<MTLRenderCommandEncoder> as void*

private:
    std::unique_ptr<MetalRenderAPIImpl> impl;
};
