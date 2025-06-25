#include "OpenGLRenderAPI.hpp"
#include "mesh.hpp"
#include "camera.hpp"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

OpenGLRenderAPI::OpenGLRenderAPI()
    : window_handle(nullptr), gl_context(nullptr), viewport_width(0), viewport_height(0), field_of_view(75.0f)
{
}

OpenGLRenderAPI::~OpenGLRenderAPI()
{
    shutdown();
}

bool OpenGLRenderAPI::initialize(WindowHandle window, int width, int height, float fov)
{
    window_handle = window;
    viewport_width = width;
    viewport_height = height;
    field_of_view = fov;

    if (!createOpenGLContext(window))
    {
        printf("Failed to create OpenGL context\n");
        return false;
    }

    setupOpenGLDefaults();
    resize(width, height);

    printf("OpenGL Render API initialized (%dx%d, FOV: %.1f)\n", width, height, fov);
    return true;
}

void OpenGLRenderAPI::shutdown()
{
    // Disable OpenGL capabilities
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_LIGHTING);
    glDisable(GL_LIGHT0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_COLOR_MATERIAL);

    destroyOpenGLContext();
}

void OpenGLRenderAPI::resize(int width, int height)
{
    viewport_width = width;
    viewport_height = height;

    float ratio = (float)width / (float)height;

    // Set up projection matrix
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(field_of_view, ratio, 0.1, 200.0);

    // Set up viewport
    glViewport(0, 0, width, height);

    // Switch back to modelview matrix
    glMatrixMode(GL_MODELVIEW);
}

bool OpenGLRenderAPI::createOpenGLContext(WindowHandle window)
{
#ifdef _WIN32
    HWND hwnd = (HWND)window;
    HDC hdc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;

    int pixel_format = ChoosePixelFormat(hdc, &pfd);
    if (!pixel_format)
    {
        printf("Failed to choose pixel format\n");
        return false;
    }

    if (!SetPixelFormat(hdc, pixel_format, &pfd))
    {
        printf("Failed to set pixel format\n");
        return false;
    }

    gl_context = wglCreateContext(hdc);
    if (!gl_context)
    {
        printf("Failed to create OpenGL context\n");
        return false;
    }

    if (!wglMakeCurrent(hdc, gl_context))
    {
        printf("Failed to make OpenGL context current\n");
        wglDeleteContext(gl_context);
        gl_context = nullptr;
        return false;
    }

    ReleaseDC(hwnd, hdc);
    return true;
#else
    // For other platforms (Linux, macOS), you'd implement X11/GLX or similar here
    printf("OpenGL context creation not implemented for this platform\n");
    return false;
#endif
}

void OpenGLRenderAPI::destroyOpenGLContext()
{
#ifdef _WIN32
    if (gl_context)
    {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(gl_context);
        gl_context = nullptr;
    }
#endif
    window_handle = nullptr;
}

void OpenGLRenderAPI::setupOpenGLDefaults()
{
    // Enable depth testing
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearDepth(1.0);

    // Enable face culling
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // Shading model
    glShadeModel(GL_SMOOTH);

    // Enable vertex arrays
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    // Enable color material for easy color changes
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);

    // Set default lighting
    enableLighting(true);
    setLighting(
        vector3f(0.2f, 0.2f, 0.2f),  // ambient
        vector3f(0.8f, 0.8f, 0.8f),  // diffuse
        vector3f(1.0f, 1.0f, 1.0f)   // position
    );
}

void OpenGLRenderAPI::beginFrame()
{
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void OpenGLRenderAPI::endFrame()
{
    // Nothing specific needed for OpenGL
}

void OpenGLRenderAPI::present()
{
#ifdef _WIN32
    if (window_handle)
    {
        HWND hwnd = (HWND)window_handle;
        HDC hdc = GetDC(hwnd);
        SwapBuffers(hdc);
        ReleaseDC(hwnd, hdc);
    }
#endif
}

void OpenGLRenderAPI::clear(const vector3f& color)
{
    glClearColor(color.X, color.Y, color.Z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void OpenGLRenderAPI::setCamera(const camera& cam)
{
    vector3f pos = cam.getPosition();
    vector3f target = cam.getTarget();
    vector3f up = cam.getUpVector();
    
    gluLookAt(
        pos.X, pos.Y, pos.Z,
        target.X, target.Y, target.Z,
        up.X, up.Y, up.Z
    );
}

void OpenGLRenderAPI::pushMatrix()
{
    glPushMatrix();
}

void OpenGLRenderAPI::popMatrix()
{
    glPopMatrix();
}

void OpenGLRenderAPI::translate(const vector3f& pos)
{
    glTranslatef(pos.X, pos.Y, pos.Z);
}

void OpenGLRenderAPI::rotate(const matrix4f& rotation)
{
    glMultMatrixf(rotation.pointer());
}

TextureHandle OpenGLRenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
{
    int width, height, channels;

    stbi_set_flip_vertically_on_load(invert_y);

    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, 0);
    if (!data)
    {
        fprintf(stderr, "Failed to load texture: %s\n", filename.c_str());
        return INVALID_TEXTURE;
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    // Determine format based on channels
    GLenum format;
    GLenum internal_format;
    switch (channels)
    {
    case 1:
        format = GL_LUMINANCE;
        internal_format = GL_LUMINANCE;
        break;
    case 3:
        format = GL_RGB;
        internal_format = GL_RGB;
        break;
    case 4:
        format = GL_RGBA;
        internal_format = GL_RGBA;
        break;
    default:
        fprintf(stderr, "Unsupported number of channels: %d\n", channels);
        stbi_image_free(data);
        glDeleteTextures(1, &texture);
        return INVALID_TEXTURE;
    }

    // Generate mipmaps if requested
    if (generate_mipmaps)
    {
        gluBuild2DMipmaps(GL_TEXTURE_2D, internal_format, width, height, format, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    else
    {
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    // Set texture wrapping
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    stbi_image_free(data);
    glBindTexture(GL_TEXTURE_2D, 0);

    return (TextureHandle)texture;
}

void OpenGLRenderAPI::bindTexture(TextureHandle texture)
{
    if (texture != INVALID_TEXTURE)
    {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
    }
    else
    {
        unbindTexture();
    }
}

void OpenGLRenderAPI::unbindTexture()
{
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLRenderAPI::deleteTexture(TextureHandle texture)
{
    if (texture != INVALID_TEXTURE)
    {
        GLuint gl_texture = (GLuint)texture;
        glDeleteTextures(1, &gl_texture);
    }
}

void OpenGLRenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0) return;

    // Apply render state before rendering
    applyRenderState(state);

    // Set up vertex arrays
    GLsizei stride = sizeof(vertex);
    GLvoid* base = (GLvoid*)&m.vertices[0];

    glVertexPointer(3, GL_FLOAT, stride, (char*)base + 0);
    glNormalPointer(GL_FLOAT, stride, (char*)base + 3 * sizeof(GLfloat));
    glTexCoordPointer(2, GL_FLOAT, stride, (char*)base + 6 * sizeof(GLfloat));

    // Set color (reset to white for textured objects)
    glColor3f(1.0f, 1.0f, 1.0f);

    // Draw the mesh
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m.vertices_len));

    // Reset some states after rendering to prevent bleeding
    if (state.blend_mode != BlendMode::None)
    {
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
}

void OpenGLRenderAPI::setRenderState(const RenderState& state)
{
    current_state = state;
    applyRenderState(state);
}

void OpenGLRenderAPI::applyRenderState(const RenderState& state)
{
    // Culling
    if (state.cull_mode == CullMode::None)
    {
        glDisable(GL_CULL_FACE);
    }
    else
    {
        glEnable(GL_CULL_FACE);
        glCullFace(getGLCullMode(state.cull_mode));
    }

    // Blending
    setupBlending(state.blend_mode);

    // Depth testing
    setupDepthTesting(state.depth_test, state.depth_write);

    // Lighting
    enableLighting(state.lighting);
}

GLenum OpenGLRenderAPI::getGLCullMode(CullMode mode)
{
    switch (mode)
    {
    case CullMode::Back: return GL_BACK;
    case CullMode::Front: return GL_FRONT;
    default: return GL_BACK;
    }
}

void OpenGLRenderAPI::setupBlending(BlendMode mode)
{
    switch (mode)
    {
    case BlendMode::None:
        glDisable(GL_BLEND);
        break;
    case BlendMode::Alpha:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case BlendMode::Additive:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    }
}

void OpenGLRenderAPI::setupDepthTesting(DepthTest test, bool write)
{
    if (test == DepthTest::None)
    {
        glDisable(GL_DEPTH_TEST);
    }
    else
    {
        glEnable(GL_DEPTH_TEST);
        switch (test)
        {
        case DepthTest::Less:
            glDepthFunc(GL_LESS);
            break;
        case DepthTest::LessEqual:
            glDepthFunc(GL_LEQUAL);
            break;
        default:
            glDepthFunc(GL_LEQUAL);
            break;
        }
    }

    glDepthMask(write ? GL_TRUE : GL_FALSE);
}

void OpenGLRenderAPI::enableLighting(bool enable)
{
    if (enable)
    {
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
    }
    else
    {
        glDisable(GL_LIGHTING);
        glDisable(GL_LIGHT0);
    }
}

void OpenGLRenderAPI::setLighting(const vector3f& ambient, const vector3f& diffuse, const vector3f& position)
{
    GLfloat light_ambient[] = { ambient.X, ambient.Y, ambient.Z, 1.0f };
    GLfloat light_diffuse[] = { diffuse.X, diffuse.Y, diffuse.Z, 1.0f };
    GLfloat light_position[] = { position.X, position.Y, position.Z, 0.0f };

    glLightfv(GL_LIGHT0, GL_AMBIENT, light_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diffuse);
    glLightfv(GL_LIGHT0, GL_POSITION, light_position);

    // Material properties
    GLfloat mat_ambient[] = { 0.2f, 0.2f, 0.2f, 1.0f };
    GLfloat mat_diffuse[] = { 0.8f, 0.8f, 0.8f, 1.0f };

    glMaterialfv(GL_FRONT, GL_AMBIENT, mat_ambient);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_diffuse);
}

// Factory implementation
IRenderAPI* CreateRenderAPI(RenderAPIType type)
{
    switch (type)
    {
    case RenderAPIType::OpenGL:
        return new OpenGLRenderAPI();
    default:
        return nullptr;
    }
}