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
    virtual void clear(const vector3f& color = vector3f(0.2f, 0.3f, 0.8f)) override;

    virtual void setCamera(const camera& cam) override;
    virtual void pushMatrix() override;
    virtual void popMatrix() override;
    virtual void translate(const vector3f& pos) override;
    virtual void rotate(const matrix4f& rotation) override;
    virtual void multiplyMatrix(const matrix4f& matrix) override;

    virtual TextureHandle loadTexture(const std::string& filename, bool invert_y = false, bool generate_mipmaps = true) override;
    virtual void bindTexture(TextureHandle texture) override;
    virtual void unbindTexture() override;
    virtual void deleteTexture(TextureHandle texture) override;

    virtual void renderMesh(const mesh& m, const RenderState& state = RenderState()) override;
    virtual void renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state = RenderState()) override;

    virtual void setRenderState(const RenderState& state) override;
    virtual void enableLighting(bool enable) override;
    virtual void setLighting(const vector3f& ambient, const vector3f& diffuse, const vector3f& direction) override;

    virtual void renderSkybox() override;

    // Shadow Mapping overrides
    virtual void beginShadowPass(const vector3f& lightDir) override;
    virtual void endShadowPass() override;
    virtual void bindShadowMap(int textureUnit) override;
    virtual matrix4f getLightSpaceMatrix() override;

    virtual IGPUMesh* createMesh() override;

    virtual const char* getAPIName() const override { return "Headless"; }
};
