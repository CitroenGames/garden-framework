// Regression tests for headless command-buffer replay accounting
// (Engine/src/Graphics/HeadlessRenderAPI.cpp replay bodies and
// RenderFrameStats in Engine/src/Graphics/RenderAPI.hpp).
// Run: ReplayTests.exe -- exit code 0 means all tests passed.

#include "Graphics/HeadlessRenderAPI.hpp"
#include "Graphics/RenderCommandBuffer.hpp"
#include "Graphics/RenderAPI.hpp"

#include <cstdint>
#include <iostream>
#include <string>

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

struct Checker
{
    std::string name;
    bool ok = true;
    void eq(const char* field, uint64_t actual, uint64_t expected)
    {
        if (actual != expected)
        {
            std::cerr << "[FAIL] " << name << ": " << field << " expected "
                      << expected << " got " << actual << std::endl;
            ok = false;
        }
    }
    void isTrue(const char* field, bool value)
    {
        if (!value)
        {
            std::cerr << "[FAIL] " << name << ": " << field << " expected true" << std::endl;
            ok = false;
        }
    }
    bool done() { return ok ? pass(name) : false; }
};

static void expectSkips(Checker& c, const RenderFrameStats& s,
                        uint64_t pso, uint64_t tex, uint64_t mesh)
{
    c.eq("binds_pso_skipped", s.binds_pso_skipped, pso);
    c.eq("binds_texture_skipped", s.binds_texture_skipped, tex);
    c.eq("binds_mesh_skipped", s.binds_mesh_skipped, mesh);
    c.eq("state_binds_skipped", s.state_binds_skipped, pso + tex + mesh);
}

static void expectAllZeroExceptBackend(Checker& c, const RenderFrameStats& s)
{
    c.isTrue("backend_name is Headless",
             s.backend_name && std::string(s.backend_name) == "Headless");
    c.isTrue("gpu_frame_ms_valid is false", !s.gpu_frame_ms_valid);
    c.eq("gpu_frame_ms is zero", static_cast<uint64_t>(s.gpu_frame_ms), 0);
    c.eq("completed_gpu_frame", s.completed_gpu_frame, 0);
    c.eq("submitted_draw_commands", s.submitted_draw_commands, 0);
    c.eq("backend_draw_calls", s.backend_draw_calls, 0);
    c.eq("instanced_batches", s.instanced_batches, 0);
    c.eq("instanced_instances", s.instanced_instances, 0);
    c.eq("parallel_replay_batches", s.parallel_replay_batches, 0);
    c.eq("parallel_replay_workers", s.parallel_replay_workers, 0);
    c.eq("state_binds_skipped", s.state_binds_skipped, 0);
    c.eq("descriptor_allocs", s.descriptor_allocs, 0);
    c.eq("descriptor_cache_hits", s.descriptor_cache_hits, 0);
    c.eq("binds_pso_skipped", s.binds_pso_skipped, 0);
    c.eq("binds_texture_skipped", s.binds_texture_skipped, 0);
    c.eq("binds_mesh_skipped", s.binds_mesh_skipped, 0);
    c.eq("pso_creates", s.pso_creates, 0);
    c.eq("pso_hits", s.pso_hits, 0);
    c.eq("upload_calls", s.upload_calls, 0);
    c.eq("upload_bytes", s.upload_bytes, 0);
}

struct Fixture
{
    HeadlessRenderAPI api;
    IGPUMesh* meshA = nullptr;
    IGPUMesh* meshB = nullptr;
    IGPUMesh* meshNotUploaded = nullptr;

    Fixture()
    {
        vertex verts[3]{};
        meshA = api.createMesh();
        meshA->uploadMeshData(verts, 3);
        meshB = api.createMesh();
        meshB->uploadMeshData(verts, 3);
        meshNotUploaded = api.createMesh();
    }
    ~Fixture()
    {
        delete meshA;
        delete meshB;
        delete meshNotUploaded;
    }
};

static glm::mat4 translated(float x)
{
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 0.0f));
}

static PSOKey cullFrontKey()
{
    PSOKey key;
    key.cull = CullMode::Front;
    return key;
}

static PSOKey blendAlphaKey()
{
    PSOKey key;
    key.blend = BlendMode::Alpha;
    return key;
}

// Records one full-mesh draw. Defaults match a plain textured opaque draw.
static void addDraw(RenderCommandBuffer& buf, IGPUMesh* mesh,
                    TextureHandle tex, bool useTex, const PSOKey& pso,
                    TextureHandle heightmap = INVALID_TEXTURE,
                    const glm::mat4& model = glm::mat4(1.0f),
                    const glm::vec3& color = glm::vec3(1.0f),
                    bool useHeightmap = false, float heightmapScale = 1.0f)
{
    buf.recordDraw(mesh, model, tex, useTex, pso, color, heightmap,
                   useHeightmap, heightmapScale, 0.0f, glm::vec2(0.0f));
}

// Mixed buffer shared by several tests. Layout (meshes A/B uploaded,
// C never uploaded):
//   0-2  A tex10 hm20 default PSO, only model matrix differs (merged run)
//   3    B tex10 hm20 default PSO
//   4    B tex11 hm20 default PSO
//   5    B tex11 hm20 cull-front PSO
//   6-7  A tex11 hm21 shadow PSO (shadow draws never merge)
//   8    null mesh (skipped at replay)
//   9    C not uploaded (skipped at replay)
//   10   A untextured (raw handle 999) hm20 default PSO
static RenderCommandBuffer buildMixedBuffer(Fixture& f)
{
    RenderCommandBuffer buf;
    const PSOKey def;
    addDraw(buf, f.meshA, 10, true, def, 20, translated(0.0f));
    addDraw(buf, f.meshA, 10, true, def, 20, translated(1.0f));
    addDraw(buf, f.meshA, 10, true, def, 20, translated(2.0f));
    addDraw(buf, f.meshB, 10, true, def, 20);
    addDraw(buf, f.meshB, 11, true, def, 20);
    addDraw(buf, f.meshB, 11, true, cullFrontKey(), 20);
    addDraw(buf, f.meshA, 11, true, PSOKey::shadowPass(), 21);
    addDraw(buf, f.meshA, 11, true, PSOKey::shadowPass(), 21, translated(1.0f));
    addDraw(buf, nullptr, 10, true, def, 20);
    addDraw(buf, f.meshNotUploaded, 10, true, def, 20);
    addDraw(buf, f.meshA, 999, false, def, 20);
    return buf;
}

static void expectMixedStats(Checker& c, const RenderFrameStats& s)
{
    c.eq("submitted_draw_commands", s.submitted_draw_commands, 11);
    c.eq("backend_draw_calls", s.backend_draw_calls, 7);
    c.eq("instanced_batches", s.instanced_batches, 1);
    c.eq("instanced_instances", s.instanced_instances, 3);
    expectSkips(c, s, 5, 6, 6);
    c.eq("descriptor_allocs", s.descriptor_allocs, 4);
    c.eq("descriptor_cache_hits", s.descriptor_cache_hits, 5);
    c.eq("pso_creates", s.pso_creates, 3);
    c.eq("pso_hits", s.pso_hits, 6);
}

static bool statsEqualExceptParallel(const RenderFrameStats& a, const RenderFrameStats& b)
{
    return a.gpu_frame_ms_valid == b.gpu_frame_ms_valid
        && a.gpu_frame_ms == b.gpu_frame_ms
        && a.completed_gpu_frame == b.completed_gpu_frame
        && a.submitted_draw_commands == b.submitted_draw_commands
        && a.backend_draw_calls == b.backend_draw_calls
        && a.instanced_batches == b.instanced_batches
        && a.instanced_instances == b.instanced_instances
        && a.state_binds_skipped == b.state_binds_skipped
        && a.descriptor_allocs == b.descriptor_allocs
        && a.descriptor_cache_hits == b.descriptor_cache_hits
        && a.binds_pso_skipped == b.binds_pso_skipped
        && a.binds_texture_skipped == b.binds_texture_skipped
        && a.binds_mesh_skipped == b.binds_mesh_skipped
        && a.pso_creates == b.pso_creates
        && a.pso_hits == b.pso_hits
        && a.upload_calls == b.upload_calls
        && a.upload_bytes == b.upload_bytes;
}

static bool testColdSingleDraw()
{
    Checker c{"cold single draw allocates one descriptor and one PSO"};
    Fixture f;
    f.api.beginFrame();
    RenderCommandBuffer buf;
    addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
    f.api.replayCommandBuffer(buf);
    RenderFrameStats s = f.api.getLastFrameStats();
    c.isTrue("backend_name is Headless",
             s.backend_name && std::string(s.backend_name) == "Headless");
    c.eq("submitted_draw_commands", s.submitted_draw_commands, 1);
    c.eq("backend_draw_calls", s.backend_draw_calls, 1);
    c.eq("instanced_batches", s.instanced_batches, 0);
    c.eq("instanced_instances", s.instanced_instances, 0);
    expectSkips(c, s, 0, 0, 0);
    c.eq("descriptor_allocs", s.descriptor_allocs, 1);
    c.eq("descriptor_cache_hits", s.descriptor_cache_hits, 0);
    c.eq("pso_creates", s.pso_creates, 1);
    c.eq("pso_hits", s.pso_hits, 0);
    c.eq("parallel_replay_batches", s.parallel_replay_batches, 0);
    c.eq("parallel_replay_workers", s.parallel_replay_workers, 0);
    return c.done();
}

static bool testColdVsWarmPerPair()
{
    Checker c{"cold frame allocates per pair and warm frame hits for the same pair"};
    Fixture f;
    RenderCommandBuffer buf;
    addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
    addDraw(buf, f.meshB, 10, true, PSOKey(), 20);

    f.api.beginFrame();
    f.api.replayCommandBuffer(buf);
    RenderFrameStats cold = f.api.getLastFrameStats();
    c.eq("cold submitted", cold.submitted_draw_commands, 2);
    c.eq("cold backend_draw_calls", cold.backend_draw_calls, 2);
    c.eq("cold descriptor_allocs", cold.descriptor_allocs, 1);
    c.eq("cold descriptor_cache_hits", cold.descriptor_cache_hits, 1);
    c.eq("cold pso_creates", cold.pso_creates, 1);
    c.eq("cold pso_hits", cold.pso_hits, 1);
    expectSkips(c, cold, 1, 1, 0);

    f.api.beginFrame();
    f.api.replayCommandBuffer(buf);
    RenderFrameStats warm = f.api.getLastFrameStats();
    c.eq("warm submitted", warm.submitted_draw_commands, 2);
    c.eq("warm backend_draw_calls", warm.backend_draw_calls, 2);
    c.eq("warm descriptor_allocs", warm.descriptor_allocs, 0);
    c.eq("warm descriptor_cache_hits", warm.descriptor_cache_hits, 2);
    c.eq("warm pso_creates", warm.pso_creates, 0);
    c.eq("warm pso_hits", warm.pso_hits, 2);
    expectSkips(c, warm, 1, 1, 0);

    // Only the heightmap half of the pair changes: one new allocation.
    RenderCommandBuffer changed;
    addDraw(changed, f.meshA, 10, true, PSOKey(), 21);
    addDraw(changed, f.meshB, 10, true, PSOKey(), 20);
    f.api.beginFrame();
    f.api.replayCommandBuffer(changed);
    RenderFrameStats third = f.api.getLastFrameStats();
    c.eq("changed-heightmap descriptor_allocs", third.descriptor_allocs, 1);
    c.eq("changed-heightmap descriptor_cache_hits", third.descriptor_cache_hits, 1);
    c.eq("changed-heightmap pso_creates", third.pso_creates, 0);
    c.eq("changed-heightmap pso_hits", third.pso_hits, 2);
    return c.done();
}

static bool testDistinctPairsCold()
{
    Checker c{"distinct texture and heightmap pairs each allocate once when cold"};
    Fixture f;
    f.api.beginFrame();
    RenderCommandBuffer buf;
    addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
    addDraw(buf, f.meshB, 10, true, PSOKey(), 21);
    addDraw(buf, f.meshA, 11, true, PSOKey(), 20);
    addDraw(buf, f.meshB, 999, false, PSOKey(), 20);
    addDraw(buf, f.meshA, INVALID_TEXTURE, true, PSOKey(), 20);
    addDraw(buf, f.meshB, 10, true, PSOKey(), 20);
    f.api.replayCommandBuffer(buf);
    RenderFrameStats s = f.api.getLastFrameStats();
    c.eq("submitted_draw_commands", s.submitted_draw_commands, 6);
    c.eq("backend_draw_calls", s.backend_draw_calls, 6);
    c.eq("descriptor_allocs", s.descriptor_allocs, 4);
    c.eq("descriptor_cache_hits", s.descriptor_cache_hits, 2);
    c.eq("pso_creates", s.pso_creates, 1);
    c.eq("pso_hits", s.pso_hits, 5);
    expectSkips(c, s, 5, 2, 0);
    return c.done();
}

static bool testFreedTextureReusedAsImplemented()
{
    Checker c{"deleted texture handle reused later still hits the retained descriptor cache"};
    Fixture f;
    RenderCommandBuffer first;
    addDraw(first, f.meshA, 42, true, PSOKey(), 7);
    f.api.beginFrame();
    f.api.replayCommandBuffer(first);
    RenderFrameStats cold = f.api.getLastFrameStats();
    c.eq("cold descriptor_allocs", cold.descriptor_allocs, 1);
    c.eq("cold descriptor_cache_hits", cold.descriptor_cache_hits, 0);

    // Headless texture management is a stub: every load returns handle 1
    // and delete is a no-op, so nothing evicts the descriptor entry.
    c.eq("loadTexture dummy handle", f.api.loadTexture("unused.png"), 1);
    const uint8_t pixels[4] = {0, 0, 0, 255};
    c.eq("loadTextureFromMemory dummy handle",
         f.api.loadTextureFromMemory(pixels, 1, 1, 4), 1);
    f.api.deleteTexture(42);
    f.api.deleteTexture(INVALID_TEXTURE);
    RenderFrameStats afterDelete = f.api.getLastFrameStats();
    c.eq("delete does not change stats", afterDelete.descriptor_allocs, 1);
    c.eq("delete does not reset submitted", afterDelete.submitted_draw_commands, 1);

    RenderCommandBuffer second;
    addDraw(second, f.meshA, 42, true, PSOKey(), 7);
    addDraw(second, f.meshB, 43, true, PSOKey(), 7);
    f.api.beginFrame();
    f.api.replayCommandBuffer(second);
    RenderFrameStats warm = f.api.getLastFrameStats();
    c.eq("reused handle descriptor_allocs", warm.descriptor_allocs, 1);
    c.eq("reused handle descriptor_cache_hits", warm.descriptor_cache_hits, 1);
    c.eq("reused handle pso_creates", warm.pso_creates, 0);
    c.eq("reused handle pso_hits", warm.pso_hits, 2);
    return c.done();
}

static bool testUntexturedShareDescriptor()
{
    Checker c{"untextured draws and INVALID texture share one descriptor pair"};
    Fixture f;
    f.api.beginFrame();
    RenderCommandBuffer buf;
    addDraw(buf, f.meshA, 999, false, PSOKey(), 5);
    addDraw(buf, f.meshB, 123, false, PSOKey(), 5);
    addDraw(buf, f.meshA, INVALID_TEXTURE, true, PSOKey(), 5);
    addDraw(buf, f.meshB, 5, true, PSOKey(), 5);
    f.api.replayCommandBuffer(buf);
    RenderFrameStats s = f.api.getLastFrameStats();
    c.eq("submitted_draw_commands", s.submitted_draw_commands, 4);
    c.eq("backend_draw_calls", s.backend_draw_calls, 4);
    c.eq("descriptor_allocs", s.descriptor_allocs, 2);
    c.eq("descriptor_cache_hits", s.descriptor_cache_hits, 2);
    c.eq("pso_creates", s.pso_creates, 1);
    c.eq("pso_hits", s.pso_hits, 3);
    expectSkips(c, s, 3, 2, 0);
    return c.done();
}

static bool testParallelSequentialParityMixed()
{
    Checker c{"parallel and sequential replay agree on a mixed buffer"};
    Fixture seqFixture;
    RenderCommandBuffer seqBuf = buildMixedBuffer(seqFixture);
    seqFixture.api.beginFrame();
    seqFixture.api.replayCommandBuffer(seqBuf);
    RenderFrameStats seq = seqFixture.api.getLastFrameStats();
    expectMixedStats(c, seq);
    c.eq("sequential parallel_replay_batches", seq.parallel_replay_batches, 0);
    c.eq("sequential parallel_replay_workers", seq.parallel_replay_workers, 0);

    Fixture parFixture;
    RenderCommandBuffer parBuf = buildMixedBuffer(parFixture);
    parFixture.api.beginFrame();
    parFixture.api.replayCommandBufferParallel(parBuf);
    RenderFrameStats par = parFixture.api.getLastFrameStats();
    expectMixedStats(c, par);
    c.eq("parallel_replay_batches", par.parallel_replay_batches, 1);
    c.eq("parallel_replay_workers", par.parallel_replay_workers, 1);
    c.isTrue("all non-parallel counters identical", statsEqualExceptParallel(seq, par));
    c.isTrue("backend names identical",
             std::string(seq.backend_name) == std::string(par.backend_name));
    return c.done();
}

static bool testMergedRunBindCounts()
{
    Checker c{"merged runs count one backend draw but visit every member for binds"};

    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, translated(0.0f));
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, translated(1.0f));
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, translated(2.0f));
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("merged submitted", s.submitted_draw_commands, 3);
        c.eq("merged backend_draw_calls", s.backend_draw_calls, 1);
        c.eq("merged instanced_batches", s.instanced_batches, 1);
        c.eq("merged instanced_instances", s.instanced_instances, 3);
        expectSkips(c, s, 2, 2, 2);
        c.eq("merged descriptor_allocs", s.descriptor_allocs, 1);
        c.eq("merged descriptor_cache_hits", s.descriptor_cache_hits, 2);
        c.eq("merged pso_creates", s.pso_creates, 1);
        c.eq("merged pso_hits", s.pso_hits, 2);
    }
    {
        // Colour differs, so no merge, but bind skipping is unchanged.
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, glm::mat4(1.0f), glm::vec3(1.0f));
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, glm::mat4(1.0f), glm::vec3(1.0f));
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("colour-break backend_draw_calls", s.backend_draw_calls, 3);
        c.eq("colour-break instanced_batches", s.instanced_batches, 0);
        c.eq("colour-break instanced_instances", s.instanced_instances, 0);
        expectSkips(c, s, 2, 2, 2);
        c.eq("colour-break descriptor_allocs", s.descriptor_allocs, 1);
        c.eq("colour-break descriptor_cache_hits", s.descriptor_cache_hits, 2);
    }
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 11, true, PSOKey::shadowPass(), 21);
        addDraw(buf, f.meshA, 11, true, PSOKey::shadowPass(), 21, translated(1.0f));
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("shadow backend_draw_calls", s.backend_draw_calls, 2);
        c.eq("shadow instanced_batches", s.instanced_batches, 0);
        expectSkips(c, s, 1, 1, 1);
        c.eq("shadow descriptor_allocs", s.descriptor_allocs, 1);
        c.eq("shadow descriptor_cache_hits", s.descriptor_cache_hits, 1);
        c.eq("shadow pso_creates", s.pso_creates, 1);
        c.eq("shadow pso_hits", s.pso_hits, 1);
    }
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 11, true, blendAlphaKey(), 21);
        addDraw(buf, f.meshA, 11, true, blendAlphaKey(), 21, translated(1.0f));
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("blend backend_draw_calls", s.backend_draw_calls, 2);
        c.eq("blend instanced_batches", s.instanced_batches, 0);
        expectSkips(c, s, 1, 1, 1);
    }
    {
        // Vertex range differs, so the run breaks even with identical binds.
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        buf.recordDrawRange(f.meshA, glm::mat4(1.0f), 10, true, PSOKey(), 0, 60);
        buf.recordDrawRange(f.meshA, glm::mat4(1.0f), 10, true, PSOKey(), 30, 60);
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("range backend_draw_calls", s.backend_draw_calls, 2);
        c.eq("range instanced_batches", s.instanced_batches, 0);
        expectSkips(c, s, 1, 1, 1);
        c.eq("range descriptor_allocs", s.descriptor_allocs, 1);
        c.eq("range descriptor_cache_hits", s.descriptor_cache_hits, 1);
    }
    return c.done();
}

static bool testCategorySkipsSumToTotal()
{
    Checker c{"per-category skipped binds sum to the total skipped binds"};
    {
        Fixture f;
        RenderCommandBuffer buf = buildMixedBuffer(f);
        f.api.beginFrame();
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        expectMixedStats(c, s);
        c.eq("mixed category sum", s.binds_pso_skipped + s.binds_texture_skipped + s.binds_mesh_skipped,
             s.state_binds_skipped);
    }
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, translated(0.0f));
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, translated(1.0f));
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, translated(2.0f));
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("merged category sum", s.binds_pso_skipped + s.binds_texture_skipped + s.binds_mesh_skipped,
             s.state_binds_skipped);
        c.eq("merged category total", s.state_binds_skipped, 6);
    }
    {
        // Alternating texture: only PSO and mesh binds are ever skipped.
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshA, 11, true, PSOKey(), 20);
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshA, 11, true, PSOKey(), 20);
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        expectSkips(c, s, 3, 0, 3);
        c.eq("alternating category sum",
             s.binds_pso_skipped + s.binds_texture_skipped + s.binds_mesh_skipped,
             s.state_binds_skipped);
    }
    return c.done();
}

static bool testStatsResetBetweenFrames()
{
    Checker c{"beginFrame resets counters while descriptor and PSO caches are retained"};
    Fixture f;
    RenderFrameStats initial = f.api.getLastFrameStats();
    expectAllZeroExceptBackend(c, initial);

    // Mutating the returned copy must not disturb the stored stats.
    initial.submitted_draw_commands = 999;
    c.eq("getLastFrameStats returns a copy",
         f.api.getLastFrameStats().submitted_draw_commands, 0);

    RenderCommandBuffer mixed = buildMixedBuffer(f);
    f.api.beginFrame();
    f.api.replayCommandBuffer(mixed);
    expectMixedStats(c, f.api.getLastFrameStats());

    f.api.beginFrame();
    expectAllZeroExceptBackend(c, f.api.getLastFrameStats());

    RenderCommandBuffer empty;
    f.api.replayCommandBuffer(empty);
    expectAllZeroExceptBackend(c, f.api.getLastFrameStats());

    RenderCommandBuffer single;
    addDraw(single, f.meshA, 10, true, PSOKey(), 20);
    f.api.replayCommandBuffer(single);
    RenderFrameStats warmSingle = f.api.getLastFrameStats();
    c.eq("retained-cache submitted", warmSingle.submitted_draw_commands, 1);
    c.eq("retained-cache backend_draw_calls", warmSingle.backend_draw_calls, 1);
    c.eq("retained-cache descriptor_allocs", warmSingle.descriptor_allocs, 0);
    c.eq("retained-cache descriptor_cache_hits", warmSingle.descriptor_cache_hits, 1);
    c.eq("retained-cache pso_creates", warmSingle.pso_creates, 0);
    c.eq("retained-cache pso_hits", warmSingle.pso_hits, 1);

    // Without beginFrame, counters accumulate across replays.
    f.api.replayCommandBuffer(single);
    RenderFrameStats accumulated = f.api.getLastFrameStats();
    c.eq("accumulated submitted", accumulated.submitted_draw_commands, 2);
    c.eq("accumulated backend_draw_calls", accumulated.backend_draw_calls, 2);
    c.eq("accumulated descriptor_cache_hits", accumulated.descriptor_cache_hits, 2);
    c.eq("accumulated pso_hits", accumulated.pso_hits, 2);

    f.api.beginFrame();
    f.api.replayCommandBufferParallel(single);
    c.eq("parallel batches before reset", f.api.getLastFrameStats().parallel_replay_batches, 1);
    f.api.beginFrame();
    expectAllZeroExceptBackend(c, f.api.getLastFrameStats());
    return c.done();
}

static bool testEmptyAndOnlyInvalidBuffers()
{
    Checker c{"empty and invalid-only buffers submit no backend work"};
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer empty;
        f.api.replayCommandBuffer(empty);
        expectAllZeroExceptBackend(c, f.api.getLastFrameStats());
    }
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer empty;
        f.api.replayCommandBufferParallel(empty);
        expectAllZeroExceptBackend(c, f.api.getLastFrameStats());
    }
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, nullptr, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshNotUploaded, 10, true, PSOKey(), 20);
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("invalid submitted", s.submitted_draw_commands, 2);
        c.eq("invalid backend_draw_calls", s.backend_draw_calls, 0);
        c.eq("invalid instanced_batches", s.instanced_batches, 0);
        expectSkips(c, s, 0, 0, 0);
        c.eq("invalid descriptor_allocs", s.descriptor_allocs, 0);
        c.eq("invalid descriptor_cache_hits", s.descriptor_cache_hits, 0);
        c.eq("invalid pso_creates", s.pso_creates, 0);
        c.eq("invalid pso_hits", s.pso_hits, 0);
    }
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, nullptr, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshNotUploaded, 10, true, PSOKey(), 20);
        f.api.replayCommandBufferParallel(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("invalid parallel submitted", s.submitted_draw_commands, 2);
        c.eq("invalid parallel backend_draw_calls", s.backend_draw_calls, 0);
        c.eq("invalid parallel batches", s.parallel_replay_batches, 1);
        c.eq("invalid parallel workers", s.parallel_replay_workers, 1);
    }
    return c.done();
}

static bool testReplayRestartsBindState()
{
    Checker c{"each replay restarts bind tracking while caches stay warm"};
    Fixture f;
    f.api.beginFrame();
    RenderCommandBuffer buf;
    addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
    addDraw(buf, f.meshB, 10, true, PSOKey(), 20);
    f.api.replayCommandBuffer(buf);
    f.api.replayCommandBuffer(buf);
    RenderFrameStats s = f.api.getLastFrameStats();
    c.eq("submitted", s.submitted_draw_commands, 4);
    c.eq("backend_draw_calls", s.backend_draw_calls, 4);
    expectSkips(c, s, 2, 2, 0);
    c.eq("descriptor_allocs", s.descriptor_allocs, 1);
    c.eq("descriptor_cache_hits", s.descriptor_cache_hits, 3);
    c.eq("pso_creates", s.pso_creates, 1);
    c.eq("pso_hits", s.pso_hits, 3);
    return c.done();
}

static bool testLargeMergedRun()
{
    Checker c{"large merged run keeps exact counters without overflow"};
    Fixture f;
    f.api.beginFrame();
    RenderCommandBuffer buf;
    buf.reserve(5000);
    for (int i = 0; i < 5000; ++i)
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, translated(static_cast<float>(i)));
    f.api.replayCommandBuffer(buf);
    RenderFrameStats s = f.api.getLastFrameStats();
    c.eq("submitted", s.submitted_draw_commands, 5000);
    c.eq("backend_draw_calls", s.backend_draw_calls, 1);
    c.eq("instanced_batches", s.instanced_batches, 1);
    c.eq("instanced_instances", s.instanced_instances, 5000);
    expectSkips(c, s, 4999, 4999, 4999);
    c.eq("descriptor_allocs", s.descriptor_allocs, 1);
    c.eq("descriptor_cache_hits", s.descriptor_cache_hits, 4999);
    c.eq("pso_creates", s.pso_creates, 1);
    c.eq("pso_hits", s.pso_hits, 4999);
    return c.done();
}

static bool testAlternatingStateCachesAreSets()
{
    Checker c{"alternating state hits retained caches even though binds are not skipped"};
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshA, 11, true, PSOKey(), 20);
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshA, 11, true, PSOKey(), 20);
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("tex-alt backend_draw_calls", s.backend_draw_calls, 4);
        expectSkips(c, s, 3, 0, 3);
        c.eq("tex-alt descriptor_allocs", s.descriptor_allocs, 2);
        c.eq("tex-alt descriptor_cache_hits", s.descriptor_cache_hits, 2);
        c.eq("tex-alt pso_creates", s.pso_creates, 1);
        c.eq("tex-alt pso_hits", s.pso_hits, 3);
    }
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshA, 10, true, cullFrontKey(), 20);
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshA, 10, true, cullFrontKey(), 20);
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("pso-alt backend_draw_calls", s.backend_draw_calls, 4);
        expectSkips(c, s, 0, 3, 3);
        c.eq("pso-alt descriptor_allocs", s.descriptor_allocs, 1);
        c.eq("pso-alt descriptor_cache_hits", s.descriptor_cache_hits, 3);
        c.eq("pso-alt pso_creates", s.pso_creates, 2);
        c.eq("pso-alt pso_hits", s.pso_hits, 2);
    }
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshB, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
        addDraw(buf, f.meshB, 10, true, PSOKey(), 20);
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("mesh-alt backend_draw_calls", s.backend_draw_calls, 4);
        expectSkips(c, s, 3, 3, 0);
        c.eq("mesh-alt descriptor_allocs", s.descriptor_allocs, 1);
        c.eq("mesh-alt descriptor_cache_hits", s.descriptor_cache_hits, 3);
    }
    return c.done();
}

static bool testHeightmapKeyIgnoresDisplacementFlag()
{
    Checker c{"descriptor pair key uses heightmap handle and ignores displacement settings"};
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, glm::mat4(1.0f),
                glm::vec3(1.0f), false);
        addDraw(buf, f.meshB, 10, true, PSOKey(), 21, glm::mat4(1.0f),
                glm::vec3(1.0f), false);
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("heightmap-differs descriptor_allocs", s.descriptor_allocs, 2);
        c.eq("heightmap-differs descriptor_cache_hits", s.descriptor_cache_hits, 0);
    }
    {
        Fixture f;
        f.api.beginFrame();
        RenderCommandBuffer buf;
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, glm::mat4(1.0f),
                glm::vec3(1.0f), false, 1.0f);
        addDraw(buf, f.meshB, 10, true, PSOKey(), 20, glm::mat4(1.0f),
                glm::vec3(1.0f), true, 1.0f);
        addDraw(buf, f.meshA, 10, true, PSOKey(), 20, glm::mat4(1.0f),
                glm::vec3(1.0f), true, 5.0f);
        f.api.replayCommandBuffer(buf);
        RenderFrameStats s = f.api.getLastFrameStats();
        c.eq("same-pair descriptor_allocs", s.descriptor_allocs, 1);
        c.eq("same-pair descriptor_cache_hits", s.descriptor_cache_hits, 2);
    }
    return c.done();
}

static bool testPsoCacheDistinctByPipelineBits()
{
    Checker c{"PSO cache creates once per distinct pipeline variant"};
    Fixture f;
    f.api.beginFrame();
    PSOKey lightingOff;
    lightingOff.lighting = false;
    PSOKey alphaTest;
    alphaTest.alpha_test = true;
    RenderCommandBuffer buf;
    addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
    addDraw(buf, f.meshA, 10, true, cullFrontKey(), 20);
    addDraw(buf, f.meshA, 10, true, blendAlphaKey(), 20);
    addDraw(buf, f.meshA, 10, true, lightingOff, 20);
    addDraw(buf, f.meshA, 10, true, alphaTest, 20);
    addDraw(buf, f.meshA, 10, true, PSOKey::depthPrepass(), 20);
    addDraw(buf, f.meshA, 10, true, PSOKey::shadowPass(), 20);
    addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
    addDraw(buf, f.meshA, 10, true, cullFrontKey(), 20);
    f.api.replayCommandBuffer(buf);
    RenderFrameStats s = f.api.getLastFrameStats();
    c.eq("submitted", s.submitted_draw_commands, 9);
    c.eq("backend_draw_calls", s.backend_draw_calls, 9);
    c.eq("pso_creates", s.pso_creates, 7);
    c.eq("pso_hits", s.pso_hits, 2);
    c.eq("descriptor_allocs", s.descriptor_allocs, 1);
    c.eq("descriptor_cache_hits", s.descriptor_cache_hits, 8);
    expectSkips(c, s, 0, 8, 8);
    return c.done();
}

static bool testEffectiveTextureBindAndDescriptor()
{
    Checker c{"texture bind and descriptor use the effective texture not the raw handle"};
    Fixture f;
    f.api.beginFrame();
    RenderCommandBuffer buf;
    addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
    addDraw(buf, f.meshA, 10, false, PSOKey(), 20);
    addDraw(buf, f.meshA, 10, true, PSOKey(), 20);
    f.api.replayCommandBuffer(buf);
    RenderFrameStats s = f.api.getLastFrameStats();
    c.eq("submitted", s.submitted_draw_commands, 3);
    c.eq("backend_draw_calls", s.backend_draw_calls, 3);
    expectSkips(c, s, 2, 0, 2);
    c.eq("descriptor_allocs", s.descriptor_allocs, 2);
    c.eq("descriptor_cache_hits", s.descriptor_cache_hits, 1);
    c.eq("pso_creates", s.pso_creates, 1);
    c.eq("pso_hits", s.pso_hits, 2);
    return c.done();
}

int main()
{
    bool ok = true;
    ok &= testColdSingleDraw();
    ok &= testColdVsWarmPerPair();
    ok &= testDistinctPairsCold();
    ok &= testFreedTextureReusedAsImplemented();
    ok &= testUntexturedShareDescriptor();
    ok &= testParallelSequentialParityMixed();
    ok &= testMergedRunBindCounts();
    ok &= testCategorySkipsSumToTotal();
    ok &= testStatsResetBetweenFrames();
    ok &= testEmptyAndOnlyInvalidBuffers();
    ok &= testReplayRestartsBindState();
    ok &= testLargeMergedRun();
    ok &= testAlternatingStateCachesAreSets();
    ok &= testHeightmapKeyIgnoresDisplacementFlag();
    ok &= testPsoCacheDistinctByPipelineBits();
    ok &= testEffectiveTextureBindAndDescriptor();
    std::cout << (ok ? "all replay tests passed" : "replay tests FAILED") << std::endl;
    return ok ? 0 : 1;
}


