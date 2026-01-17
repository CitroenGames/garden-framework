#pragma once

#include "RenderAPI.hpp"

class HeadlessRenderAPI : public IRenderAPI
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

    virtual TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) override;
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

    virtual const char* getAPIName() const override { return "Headless"; }
};
