#pragma once

#include "RenderAPI.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <GL/gl.h>
#include <GL/glu.h>

#ifdef _WIN32
typedef HGLRC OpenGLContext;
#else
typedef void* OpenGLContext; // For other platforms
#endif

class OpenGLRenderAPI : public IRenderAPI
{
private:
    WindowHandle window_handle;
    OpenGLContext gl_context;
    int viewport_width;
    int viewport_height;
    float field_of_view;
    RenderState current_state;

    // Internal helper methods
    bool createOpenGLContext(WindowHandle window);
    void destroyOpenGLContext();
    void setupOpenGLDefaults();
    void applyRenderState(const RenderState& state);
    GLenum getGLCullMode(CullMode mode);
    void setupBlending(BlendMode mode);
    void setupDepthTesting(DepthTest test, bool write);

public:
    OpenGLRenderAPI();
    virtual ~OpenGLRenderAPI();

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

    virtual void setRenderState(const RenderState& state) override;
    virtual void enableLighting(bool enable) override;
    virtual void setLighting(const vector3f& ambient, const vector3f& diffuse, const vector3f& position) override;

    virtual const char* getAPIName() const override { return "OpenGL"; }
};