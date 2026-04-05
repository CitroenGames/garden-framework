#ifdef __APPLE__

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "RmlRenderer_Metal.h"
#include "Graphics/MetalRenderAPI.hpp"
#include "Utils/EnginePaths.hpp"

#include "stb_image.h"

#include <cstring>
#include <unordered_map>

struct RmlRenderer_Metal::Impl {
    MetalRenderAPI* renderAPI = nullptr;
    id<MTLDevice> device = nil;
    id<MTLRenderCommandEncoder> encoder = nil;

    id<MTLRenderPipelineState> pipelineTextured = nil;
    id<MTLRenderPipelineState> pipelineColor = nil;
    id<MTLDepthStencilState> depthStencilState = nil;
    id<MTLSamplerState> sampler = nil;
    id<MTLLibrary> shaderLibrary = nil;

    struct UniformData {
        float transform[16];
        float translation[2];
        float padding[2];
    };

    struct GeometryData {
        id<MTLBuffer> vertexBuffer;
        id<MTLBuffer> indexBuffer;
        int numIndices;
    };
    uintptr_t nextGeometryHandle = 1;
    std::unordered_map<uintptr_t, GeometryData> geometries;

    struct TextureData {
        id<MTLTexture> texture;
    };
    uintptr_t nextTextureHandle = 1;
    std::unordered_map<uintptr_t, TextureData> textures;

    int viewportWidth = 0;
    int viewportHeight = 0;
    bool scissorEnabled = false;
    bool transformEnabled = false;
    Rml::Matrix4f transform;
};

RmlRenderer_Metal::RmlRenderer_Metal() : m_impl(new Impl()) {}
RmlRenderer_Metal::~RmlRenderer_Metal() { Shutdown(); delete m_impl; }

bool RmlRenderer_Metal::Init(MetalRenderAPI* renderAPI)
{
    m_impl->renderAPI = renderAPI;
    m_impl->device = (__bridge id<MTLDevice>)renderAPI->getDevice();
    if (!m_impl->device)
        return false;

    // Load shader
    // Load Slang-generated Metal shader sources and concatenate
    NSMutableString* source = [NSMutableString string];
    [source appendString:@"#include <metal_stdlib>\nusing namespace metal;\n\n"];
    NSArray* rmlShaderFiles = @[
        [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/rmlui_vertex.metal").c_str()],
        [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/rmlui_fragment_textured.metal").c_str()],
        [NSString stringWithUTF8String:EnginePaths::resolveEngineAsset("../assets/shaders/compiled/metal/rmlui_fragment_color.metal").c_str()]];
    for (NSString* path in rmlShaderFiles) {
        NSString* src = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
        if (src) {
            NSString* stripped = [src stringByReplacingOccurrencesOfString:@"#include <metal_stdlib>" withString:@""];
            stripped = [stripped stringByReplacingOccurrencesOfString:@"using namespace metal;" withString:@""];
            [source appendFormat:@"\n%@\n", stripped];
        }
    }
    NSError* error = nil;
    MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
    m_impl->shaderLibrary = [m_impl->device newLibraryWithSource:source options:opts error:&error];
    if (!m_impl->shaderLibrary) {
        printf("[RmlUi Metal] Shader compilation failed: %s\n", [[error localizedDescription] UTF8String]);
        return false;
    }

    id<MTLFunction> vertFunc = [m_impl->shaderLibrary newFunctionWithName:@"rmlui_vertex"];
    id<MTLFunction> fragTexFunc = [m_impl->shaderLibrary newFunctionWithName:@"rmlui_fragment_textured"];
    id<MTLFunction> fragColFunc = [m_impl->shaderLibrary newFunctionWithName:@"rmlui_fragment_color"];

    if (!vertFunc || !fragTexFunc || !fragColFunc) {
        printf("[RmlUi Metal] Failed to find shader functions\n");
        return false;
    }

    // Vertex descriptor matching Rml::Vertex layout
    MTLVertexDescriptor* vtxDesc = [[MTLVertexDescriptor alloc] init];
    // position: float2
    vtxDesc.attributes[0].format = MTLVertexFormatFloat2;
    vtxDesc.attributes[0].offset = offsetof(Rml::Vertex, position);
    vtxDesc.attributes[0].bufferIndex = 0;
    // color: uchar4 normalized
    vtxDesc.attributes[1].format = MTLVertexFormatUChar4Normalized;
    vtxDesc.attributes[1].offset = offsetof(Rml::Vertex, colour);
    vtxDesc.attributes[1].bufferIndex = 0;
    // texcoord: float2
    vtxDesc.attributes[2].format = MTLVertexFormatFloat2;
    vtxDesc.attributes[2].offset = offsetof(Rml::Vertex, tex_coord);
    vtxDesc.attributes[2].bufferIndex = 0;
    vtxDesc.layouts[0].stride = sizeof(Rml::Vertex);
    vtxDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    // Textured pipeline
    {
        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = vertFunc;
        desc.fragmentFunction = fragTexFunc;
        desc.vertexDescriptor = vtxDesc;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        // Premultiplied alpha
        desc.colorAttachments[0].blendingEnabled = YES;
        desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

        m_impl->pipelineTextured = [m_impl->device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!m_impl->pipelineTextured) {
            printf("[RmlUi Metal] Failed to create textured pipeline: %s\n", [[error localizedDescription] UTF8String]);
            return false;
        }
    }

    // Color-only pipeline
    {
        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = vertFunc;
        desc.fragmentFunction = fragColFunc;
        desc.vertexDescriptor = vtxDesc;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.colorAttachments[0].blendingEnabled = YES;
        desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

        m_impl->pipelineColor = [m_impl->device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!m_impl->pipelineColor) {
            printf("[RmlUi Metal] Failed to create color pipeline: %s\n", [[error localizedDescription] UTF8String]);
            return false;
        }
    }

    // Depth stencil state (disabled)
    MTLDepthStencilDescriptor* dsDesc = [[MTLDepthStencilDescriptor alloc] init];
    dsDesc.depthCompareFunction = MTLCompareFunctionAlways;
    dsDesc.depthWriteEnabled = NO;
    m_impl->depthStencilState = [m_impl->device newDepthStencilStateWithDescriptor:dsDesc];

    // Sampler
    MTLSamplerDescriptor* sampDesc = [[MTLSamplerDescriptor alloc] init];
    sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
    sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
    sampDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    m_impl->sampler = [m_impl->device newSamplerStateWithDescriptor:sampDesc];

    return true;
}

void RmlRenderer_Metal::Shutdown()
{
    m_impl->geometries.clear();
    m_impl->textures.clear();
    m_impl->pipelineTextured = nil;
    m_impl->pipelineColor = nil;
    m_impl->depthStencilState = nil;
    m_impl->sampler = nil;
    m_impl->shaderLibrary = nil;
    m_impl->device = nil;
    m_impl->renderAPI = nullptr;
}

void RmlRenderer_Metal::SetViewport(int width, int height)
{
    m_impl->viewportWidth = width;
    m_impl->viewportHeight = height;
}

void RmlRenderer_Metal::BeginFrame()
{
    m_impl->encoder = (__bridge id<MTLRenderCommandEncoder>)m_impl->renderAPI->getRenderCommandEncoder();
}

Rml::CompiledGeometryHandle RmlRenderer_Metal::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    Impl::GeometryData geo;
    geo.numIndices = (int)indices.size();
    geo.vertexBuffer = [m_impl->device newBufferWithBytes:vertices.data()
                                                   length:vertices.size() * sizeof(Rml::Vertex)
                                                  options:MTLResourceStorageModeShared];
    geo.indexBuffer = [m_impl->device newBufferWithBytes:indices.data()
                                                  length:indices.size() * sizeof(int)
                                                 options:MTLResourceStorageModeShared];
    if (!geo.vertexBuffer || !geo.indexBuffer)
        return 0;

    uintptr_t handle = m_impl->nextGeometryHandle++;
    m_impl->geometries[handle] = geo;
    return (Rml::CompiledGeometryHandle)handle;
}

void RmlRenderer_Metal::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    auto it = m_impl->geometries.find((uintptr_t)handle);
    if (it == m_impl->geometries.end() || !m_impl->encoder)
        return;

    const auto& geo = it->second;

    // Build uniform data
    Impl::UniformData uniforms = {};

    float L = 0.0f, R = (float)m_impl->viewportWidth;
    float T = 0.0f, B = (float)m_impl->viewportHeight;
    float ortho[16] = {
        2.0f / (R - L),    0.0f,              0.0f, 0.0f,
        0.0f,              2.0f / (T - B),    0.0f, 0.0f,
        0.0f,              0.0f,              0.5f, 0.0f,
        (L + R) / (L - R), (T + B) / (B - T), 0.5f, 1.0f
    };

    if (m_impl->transformEnabled)
    {
        const float* b = m_impl->transform.data();
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                uniforms.transform[j * 4 + i] = 0.0f;
                for (int k = 0; k < 4; k++)
                    uniforms.transform[j * 4 + i] += ortho[k * 4 + i] * b[j * 4 + k];
            }
    }
    else
    {
        memcpy(uniforms.transform, ortho, sizeof(ortho));
    }

    uniforms.translation[0] = translation.x;
    uniforms.translation[1] = translation.y;

    // Set state
    if (texture)
        [m_impl->encoder setRenderPipelineState:m_impl->pipelineTextured];
    else
        [m_impl->encoder setRenderPipelineState:m_impl->pipelineColor];

    [m_impl->encoder setDepthStencilState:m_impl->depthStencilState];
    [m_impl->encoder setCullMode:MTLCullModeNone];

    // Bind vertex buffer and uniforms
    [m_impl->encoder setVertexBuffer:geo.vertexBuffer offset:0 atIndex:0];
    [m_impl->encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];

    // Bind texture if present
    if (texture)
    {
        auto tex_it = m_impl->textures.find((uintptr_t)texture);
        if (tex_it != m_impl->textures.end())
        {
            [m_impl->encoder setFragmentTexture:tex_it->second.texture atIndex:0];
            [m_impl->encoder setFragmentSamplerState:m_impl->sampler atIndex:0];
        }
    }

    // Set scissor
    if (m_impl->scissorEnabled)
    {
        // Scissor was already set by SetScissorRegion
    }
    else
    {
        MTLScissorRect fullRect = { 0, 0, (NSUInteger)m_impl->viewportWidth, (NSUInteger)m_impl->viewportHeight };
        [m_impl->encoder setScissorRect:fullRect];
    }

    // Draw indexed
    [m_impl->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:geo.numIndices
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:geo.indexBuffer
                         indexBufferOffset:0];
}

void RmlRenderer_Metal::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
    m_impl->geometries.erase((uintptr_t)handle);
}

Rml::TextureHandle RmlRenderer_Metal::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
    int w, h, channels;
    unsigned char* data = stbi_load(source.c_str(), &w, &h, &channels, 4);
    if (!data)
        return 0;

    texture_dimensions.x = w;
    texture_dimensions.y = h;

    // Premultiply alpha
    for (int i = 0; i < w * h; i++)
    {
        unsigned char* p = data + i * 4;
        float a = p[3] / 255.0f;
        p[0] = (unsigned char)(p[0] * a);
        p[1] = (unsigned char)(p[1] * a);
        p[2] = (unsigned char)(p[2] * a);
    }

    auto handle = GenerateTexture(Rml::Span<const Rml::byte>(data, w * h * 4), texture_dimensions);
    stbi_image_free(data);
    return handle;
}

Rml::TextureHandle RmlRenderer_Metal::GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions)
{
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                   width:source_dimensions.x
                                                                                  height:source_dimensions.y
                                                                               mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;

    id<MTLTexture> texture = [m_impl->device newTextureWithDescriptor:desc];
    if (!texture)
        return 0;

    MTLRegion region = MTLRegionMake2D(0, 0, source_dimensions.x, source_dimensions.y);
    [texture replaceRegion:region mipmapLevel:0 withBytes:source_data.data() bytesPerRow:source_dimensions.x * 4];

    Impl::TextureData tex;
    tex.texture = texture;

    uintptr_t handle = m_impl->nextTextureHandle++;
    m_impl->textures[handle] = tex;
    return (Rml::TextureHandle)handle;
}

void RmlRenderer_Metal::ReleaseTexture(Rml::TextureHandle texture)
{
    m_impl->textures.erase((uintptr_t)texture);
}

void RmlRenderer_Metal::EnableScissorRegion(bool enable)
{
    m_impl->scissorEnabled = enable;
}

void RmlRenderer_Metal::SetScissorRegion(Rml::Rectanglei region)
{
    if (!m_impl->encoder)
        return;

    MTLScissorRect rect;
    rect.x = region.Left();
    rect.y = region.Top();
    rect.width = region.Width();
    rect.height = region.Height();
    [m_impl->encoder setScissorRect:rect];
}

void RmlRenderer_Metal::SetTransform(const Rml::Matrix4f* transform)
{
    if (transform)
    {
        m_impl->transformEnabled = true;
        m_impl->transform = *transform;
    }
    else
    {
        m_impl->transformEnabled = false;
    }
}

#endif // __APPLE__
