// Renderer-perf tests: packed sort-key order, depth-bucketed opaque
// sort, and Headless counting replay (redundant-bind skip verification).
// Run: PerfTests.exe -> exit code 0 means all tests passed.

#include "Graphics/RenderCommandBuffer.hpp"
#include "Graphics/HeadlessRenderAPI.hpp"
#include "Graphics/IGPUMesh.hpp"
#include "Graphics/ForwardPlusLightBinning.hpp"
#include "Utils/Vertex.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

static bool fail(const std::string& name, const std::string& reason)
{
    std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}

static bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl;
    return true;
}

static DrawCommand makeCmd(const PSOKey& pso, uint32_t material = 0,
                           bool useTexture = false, TextureHandle tex = INVALID_TEXTURE,
                           float z = -10.0f)
{
    DrawCommand cmd;
    cmd.pso_key = pso;
    cmd.material_flags = material;
    cmd.use_texture = useTexture;
    cmd.texture = tex;
    cmd.model_matrix = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, z));
    return cmd;
}

// Packed key must order pipeline > material > lighting > geometry > depth.
static bool testPackedKeyFieldPriority()
{
    const std::string name = "packed sort key orders pipeline>material>lighting>geometry>depth";

    PSOKey main;
    PSOKey shadow = PSOKey::shadowPass();

    // Pipeline dominates everything below it.
    DrawCommand lo = makeCmd(main, 0xFFFFu, true, 7);
    lo.use_heightmap_displacement = true;
    lo.vertex_count = 64;
    DrawCommand hi = makeCmd(shadow, 0u);
    if (!(lo.packedSortKey(0xFFFu) < hi.packedSortKey(0u)))
        return fail(name, "shadow pipeline did not dominate material/lighting/geometry/depth");

    // Material dominates lighting/geometry/depth.
    DrawCommand matLo = makeCmd(main, 0u, true, 7);
    DrawCommand matHi = makeCmd(main, 1u, false);
    if (!(matLo.packedSortKey(0xFFFu) < matHi.packedSortKey(0u)))
        return fail(name, "material did not dominate lighting/geometry/depth");

    // Lighting (use_texture bit) dominates geometry/depth.
    DrawCommand litHi = makeCmd(main, 0u, true, 7);
    DrawCommand litLo = makeCmd(main, 0u, false);
    litLo.use_heightmap_displacement = true;
    litLo.vertex_count = 64;
    if (!(litLo.packedSortKey(0xFFFu) < litHi.packedSortKey(0u)))
        return fail(name, "lighting did not dominate geometry/depth");

    // Geometry dominates depth: smaller geometry wins even with max depth.
    DrawCommand geoHi = makeCmd(main, 0u, true, 7);
    geoHi.use_heightmap_displacement = true;
    geoHi.vertex_count = 64;
    DrawCommand geoLo = makeCmd(main, 0u, true, 7);
    if (!(geoLo.packedSortKey(0xFFFu) < geoHi.packedSortKey(0u)))
        return fail(name, "geometry did not dominate depth");

    // Depth bucket itself is monotonic.
    DrawCommand d = makeCmd(main);
    if (!(d.packedSortKey(5) < d.packedSortKey(6)))
        return fail(name, "depth bucket not monotonic");
    // Bucket is masked to 12 bits.
    if (d.packedSortKey(0x1FFFu) != d.packedSortKey(0xFFFu))
        return fail(name, "depth bucket not masked to 12 bits");

    return pass(name);
}

static bool testSortGroupsByPipeline()
{
    const std::string name = "sort() groups draws by packed pipeline key";
    PSOKey main;
    PSOKey shadow = PSOKey::shadowPass();

    RenderCommandBuffer buf;
    for (int i = 0; i < 6; i++)
    {
        DrawCommand s = makeCmd(shadow, (uint32_t)(i % 2));
        s.model_matrix = glm::translate(glm::mat4(1.0f), glm::vec3((float)i, 0.0f, -10.0f));
        buf.recordDrawRangePBR(nullptr, s.model_matrix, INVALID_TEXTURE, false,
                               s.pso_key, 0, 0, glm::vec3(1.0f), 0.0f,
                               0.0f, 0.5f, glm::vec3(0.0f),
                               INVALID_TEXTURE, false, 1.0f, 0.0f,
                               glm::vec2(0.0f), s.material_flags);
        DrawCommand m = makeCmd(main, (uint32_t)(i % 3), true, (TextureHandle)(7 + i % 2));
        buf.recordDrawRangePBR(nullptr, m.model_matrix, m.texture, true,
                               m.pso_key, 0, 0, glm::vec3(1.0f), 0.0f,
                               0.0f, 0.5f, glm::vec3(0.0f),
                               INVALID_TEXTURE, false, 1.0f, 0.0f,
                               glm::vec2(0.0f), m.material_flags);
    }
    buf.sort();
    for (size_t i = 1; i < buf.size(); i++)
    {
        if (buf[i - 1].packedSortKey() > buf[i].packedSortKey())
            return fail(name, "buffer not ordered by packed key after sort()");
    }
    return pass(name);
}

static bool testSortWithViewDepthFrontToBack()
{
    const std::string name = "sortWithViewDepth() orders opaques front-to-back within a group";
    PSOKey main;
    PSOKey shadow = PSOKey::shadowPass();
    glm::mat4 view(1.0f);

    RenderCommandBuffer buf;
    for (float z : {-30.0f, -10.0f, -20.0f})
        buf.recordDraw(nullptr, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, z)),
                       INVALID_TEXTURE, false, main);
    // Nearest draw but shadow pipeline: must sort after all main draws.
    buf.recordDraw(nullptr, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
                   INVALID_TEXTURE, false, shadow);

    buf.sortWithViewDepth(view);
    if (buf.size() != 4)
        return fail(name, "unexpected command count after depth sort");

    float lastDepth = -1.0f;
    for (size_t i = 0; i < 3; i++)
    {
        if (buf[i].pso_key.shadow)
            return fail(name, "shadow draw sorted inside main group");
        float z = buf[i].model_matrix[3].z;
        float depth = -z;
        if (depth < lastDepth)
            return fail(name, "main draws not front-to-back");
        lastDepth = depth;
    }
    if (!buf[3].pso_key.shadow)
        return fail(name, "shadow draw did not sort after main group");
    return pass(name);
}

static IGPUMesh* makeUploadedMesh(HeadlessRenderAPI& api)
{
    IGPUMesh* mesh = api.createMesh();
    vertex verts[3] = {};
    mesh->uploadMeshData(verts, 3);
    return mesh;
}

static bool testHeadlessReplaySkipsRedundantBinds()
{
    const std::string name = "headless replay binds once for identical state";
    HeadlessRenderAPI api;
    api.beginFrame();
    IGPUMesh* mesh = makeUploadedMesh(api);

    PSOKey main;
    RenderCommandBuffer buf;
    for (int i = 0; i < 8; i++)
        buf.recordDraw(mesh, glm::mat4(1.0f), 7, true, main);

    api.replayCommandBuffer(buf);
    RenderFrameStats stats = api.getLastFrameStats();

    if (stats.backend_draw_calls != 8)
        return fail(name, "expected 8 backend draws, got " + std::to_string(stats.backend_draw_calls));
    // 7 PSO skips + 7 texture skips + 7 mesh skips.
    if (stats.state_binds_skipped != 21)
        return fail(name, "expected 21 skipped binds, got " + std::to_string(stats.state_binds_skipped));
    return pass(name);
}

static bool testHeadlessReplayAlternatingTexture()
{
    const std::string name = "headless replay rebinds only on state change";
    HeadlessRenderAPI api;
    api.beginFrame();
    IGPUMesh* mesh = makeUploadedMesh(api);

    PSOKey main;
    RenderCommandBuffer buf;
    for (int i = 0; i < 8; i++)
        buf.recordDraw(mesh, glm::mat4(1.0f), (TextureHandle)(7 + (i % 2)), true, main);

    api.replayCommandBuffer(buf);
    RenderFrameStats stats = api.getLastFrameStats();

    if (stats.backend_draw_calls != 8)
        return fail(name, "expected 8 backend draws, got " + std::to_string(stats.backend_draw_calls));
    // Texture changes every draw (0 skips); PSO + mesh skip 7 each.
    if (stats.state_binds_skipped != 14)
        return fail(name, "expected 14 skipped binds, got " + std::to_string(stats.state_binds_skipped));
    return pass(name);
}

static void printTiming(const char* label, size_t ops, std::chrono::steady_clock::duration elapsed)
{
    const double totalNs = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const double nsPerOp = ops ? totalNs / (double)ops : 0.0;
    std::cout << "[BENCH] " << label << " ops=" << ops << " ns/op=" << nsPerOp << std::endl;
}

static RenderCommandBuffer makeUnsortedBuffer(IGPUMesh* mesh, size_t count)
{
    RenderCommandBuffer buf;
    buf.reserve(count);
    PSOKey main;
    for (size_t i = 0; i < count; i++)
    {
        const uint32_t scrambled = (uint32_t)((i * 2654435761u) >> 8);
        const glm::mat4 model = glm::translate(glm::mat4(1.0f),
            glm::vec3(0.0f, 0.0f, -1.0f - (float)(scrambled % 97u)));
        const PSOKey key = (scrambled & 0x100u) ? PSOKey::shadowPass() : main;
        buf.recordDrawRangePBR(mesh, model, (TextureHandle)(7u + (scrambled & 7u)), true,
                               key, 0, 0, glm::vec3(1.0f), 0.0f, 0.0f, 0.5f,
                               glm::vec3(0.0f), INVALID_TEXTURE, false, 1.0f, 0.0f,
                               glm::vec2(0.0f), scrambled & 3u);
    }
    return buf;
}

static bool benchSort(size_t count)
{
    const std::string name = "timed sort() stays ordered";
    HeadlessRenderAPI api;
    IGPUMesh* mesh = makeUploadedMesh(api);
    RenderCommandBuffer buf = makeUnsortedBuffer(mesh, count);
    const auto start = std::chrono::steady_clock::now();
    buf.sort();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    printTiming("sort", count, elapsed);
    for (size_t i = 1; i < buf.size(); i++)
        if (buf[i - 1].packedSortKey() > buf[i].packedSortKey())
            return fail(name, "buffer not ordered after timed sort()");
    delete mesh;
    return pass(name + " (" + std::to_string(count) + ")");
}

static bool benchSortWithViewDepth(size_t count)
{
    const std::string name = "timed depth-bucketed sort stays ordered";
    HeadlessRenderAPI api;
    IGPUMesh* mesh = makeUploadedMesh(api);
    RenderCommandBuffer buf = makeUnsortedBuffer(mesh, count);
    const glm::mat4 view(1.0f);
    const auto start = std::chrono::steady_clock::now();
    buf.sortWithViewDepth(view);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    printTiming("sortWithViewDepth", count, elapsed);
    if (buf.size() != count)
        return fail(name, "command count changed during timed sort");
    delete mesh;
    return pass(name + " (" + std::to_string(count) + ")");
}

static bool benchRecordDraw(size_t count)
{
    const std::string name = "timed recordDraw records every draw";
    HeadlessRenderAPI api;
    IGPUMesh* mesh = makeUploadedMesh(api);
    RenderCommandBuffer buf;
    buf.reserve(count);
    PSOKey main;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < count; i++)
        buf.recordDraw(mesh, glm::translate(glm::mat4(1.0f), glm::vec3((float)i, 0.0f, -10.0f)),
                       7, true, main);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    printTiming("recordDraw", count, elapsed);
    if (buf.size() != count)
        return fail(name, "recorded count does not match requested count");
    delete mesh;
    return pass(name + " (" + std::to_string(count) + ")");
}

static bool benchBinForwardPlusLights(size_t count)
{
    const std::string name = "timed light binning bins every visible light";
    std::vector<GPUPointLight> lights(count);
    for (size_t i = 0; i < count; i++)
    {
        lights[i].position = glm::vec3((float)((int)(i % 16) - 8) * 0.4f,
                                       (float)((int)((i / 16) % 16) - 8) * 0.4f,
                                       -5.0f - (float)(i % 8));
        lights[i].range = 0.5f;
    }
    const glm::mat4 view(1.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
    const auto start = std::chrono::steady_clock::now();
    ForwardPlusBinningResult result = binForwardPlusLights(
        lights.data(), (uint32_t)count, nullptr, 0, view, proj, 160, 160, 0.1f, 1000.0f, true, true);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    printTiming("binForwardPlusLights", count, elapsed);
    if (result.lightIndices.empty())
        return fail(name, "no lights were binned");
    return pass(name + " (" + std::to_string(count) + ")");
}

static bool testHeadlessFrameStatsPopulate()
{
    const std::string name = "counting replay reports descriptor, parallel and instanced counters";
    HeadlessRenderAPI api;
    IGPUMesh* mesh = makeUploadedMesh(api);
    PSOKey main;
    RenderCommandBuffer buf;
    for (int i = 0; i < 8; i++)
        buf.recordDraw(mesh, glm::mat4(1.0f), 7, true, main);

    api.beginFrame();
    api.replayCommandBuffer(buf);
    RenderFrameStats first = api.getLastFrameStats();
    if (first.backend_draw_calls != 8 || first.submitted_draw_commands != 8)
        return fail(name, "draw counters did not populate on first replay");
    if (first.descriptor_allocs != 1 || first.descriptor_cache_hits != 7)
        return fail(name, "expected 1 descriptor alloc and 7 hits, got allocs=" +
                            std::to_string(first.descriptor_allocs) + " hits=" +
                            std::to_string(first.descriptor_cache_hits));

    api.beginFrame();
    api.replayCommandBuffer(buf);
    RenderFrameStats second = api.getLastFrameStats();
    if (second.descriptor_allocs != 0 || second.descriptor_cache_hits != 8)
        return fail(name, "expected warm replay with 0 allocs and 8 hits, got allocs=" +
                            std::to_string(second.descriptor_allocs) + " hits=" +
                            std::to_string(second.descriptor_cache_hits));
    if (second.instanced_batches != 0 || second.instanced_instances != 0)
        return fail(name, "plain draws must not be reported as instanced draws");

    api.beginFrame();
    api.replayCommandBufferParallel(buf);
    RenderFrameStats parallel = api.getLastFrameStats();
    if (parallel.parallel_replay_batches < 1 || parallel.parallel_replay_workers < 1)
        return fail(name, "parallel replay counters did not populate");
    if (parallel.backend_draw_calls != first.backend_draw_calls ||
        parallel.state_binds_skipped != first.state_binds_skipped)
        return fail(name, "parallel replay counts differ from sequential replay");
    delete mesh;
    return pass(name);
}
int main()
{
    bool ok = true;
    ok &= testPackedKeyFieldPriority();
    ok &= testSortGroupsByPipeline();
    ok &= testSortWithViewDepthFrontToBack();
    ok &= testHeadlessReplaySkipsRedundantBinds();
    ok &= testHeadlessReplayAlternatingTexture();
    ok &= testHeadlessFrameStatsPopulate();
    ok &= benchSort(1000);
    ok &= benchSort(10000);
    ok &= benchSortWithViewDepth(1000);
    ok &= benchSortWithViewDepth(10000);
    ok &= benchRecordDraw(1000);
    ok &= benchRecordDraw(10000);
    ok &= benchBinForwardPlusLights(1000);
    ok &= benchBinForwardPlusLights(10000);
    std::cout << (ok ? "all perf tests passed" : "perf tests FAILED") << std::endl;
    return ok ? 0 : 1;
}
