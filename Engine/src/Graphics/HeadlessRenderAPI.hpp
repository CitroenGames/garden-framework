#pragma once

#include "RenderAPI.hpp"
#include <cstdint>
#include <unordered_set>

class ENGINE_API HeadlessRenderAPI : public IRenderAPI
{
public:
    HeadlessRenderAPI();
    virtual ~HeadlessRenderAPI();

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
    virtual void bindTexture(TextureHandle texture) override;
    virtual void unbindTexture() override;
    virtual void deleteTexture(TextureHandle texture) override;

    virtual void renderMesh(const mesh& m, const RenderState& state = RenderState()) override;
    virtual void renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state = RenderState()) override;

    virtual void setRenderState(const RenderState& state) override;
    virtual void enableLighting(bool enable) override;
    virtual void setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction) override;

    virtual void renderSkybox() override;

    // Shadow Mapping overrides (CSM)
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

    // Counting replay -- simulates backend bind caching (PSO /
    // texture / mesh) so sort-key grouping and redundant-bind skip behaviour
    // can be verified without a GPU.
    // m_descriptorCache persists across frames (never cleared by
    // beginFrame), mirroring a cross-frame descriptor cache.
    virtual void replayCommandBuffer(const RenderCommandBuffer& cmds) override;
    virtual void replayCommandBufferParallel(const RenderCommandBuffer& cmds) override;
    virtual RenderFrameStats getLastFrameStats() const override;

    // Headless has no depth target. Refuse before any world-frame state can change.
    bool beginViewmodelPass(const ViewmodelSettings&) override { return false; }
    void endViewmodelPass() override {}

private:
    RenderFrameStats m_lastFrameStats{};
    std::unordered_set<uint64_t> m_descriptorCache{};
    std::unordered_set<uint64_t> m_psoCache{};

    virtual const char* getAPIName() const override { return "Headless"; }

    // Graphics settings (no-op for headless)
    virtual void setFXAAEnabled(bool enabled) override {}
    virtual bool isFXAAEnabled() const override { return false; }
    virtual void setShadowQuality(int quality) override {}
    virtual int getShadowQuality() const override { return 0; }

    // PIE viewport render targets (no-op for headless)
    int createPIEViewport(int, int) override { return -1; }
    void destroyPIEViewport(int) override {}
    void destroyAllPIEViewports() override {}
    void setPIEViewportSize(int, int, int) override {}
    void setActiveSceneTarget(int) override {}
    uint64_t getPIEViewportTextureID(int) override { return 0; }
};
