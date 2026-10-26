// Tests for RenderGraph compilation and execution bookkeeping
// (Engine/src/Graphics/RenderGraph). A counting fake RGBackend records
// every backend call, so no GPU is involved. Run: RenderGraphTests.exe,
// exit code 0 means all tests passed.
//
// Link note: RenderGraph/RGCompiler/RGBuilder live in EngineGraphics, but
// their classes carry no DLL export, so linking EngineGraphics cannot
// resolve them (verified: no RenderGraph symbols in EngineGraphics.dll
// exports). This suite therefore compiles the three RenderGraph
// translation units directly (see RenderGraphTests.buildscript) and links
// EngineCore for the public include paths, exactly like sibling suites.

#include "Graphics/RenderGraph/RenderGraph.hpp"

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

struct FakeContext : RGContext {};

struct RecordedBarrier {
    RGResourceHandle handle;
    RGResourceUsage from;
    RGResourceUsage to;
};

// Counting fake backend: records creates, destroys, barriers and flushes.
struct FakeBackend : RGBackend {
    FakeContext ctx;
    int beginCount = 0;
    int endCount = 0;
    int flushCount = 0;
    std::vector<RGResourceHandle> created;
    std::vector<RGTextureDesc> createdDescs;
    std::vector<RGResourceHandle> destroyed;
    std::vector<RecordedBarrier> barriers;

    void createTransientTexture(RGResourceHandle handle, const RGTextureDesc& desc) override
    {
        created.push_back(handle);
        createdDescs.push_back(desc);
    }
    void destroyTransientTexture(RGResourceHandle handle) override { destroyed.push_back(handle); }
    void insertBarrier(RGResourceHandle handle, RGResourceUsage fromUsage, RGResourceUsage toUsage) override
    {
        barriers.push_back({handle, fromUsage, toUsage});
    }
    void flushBarriers() override { ++flushCount; }
    RGContext& getContext() override { return ctx; }
    void beginFrame() override { ++beginCount; }
    void endFrame() override { ++endCount; }
};

static RGTextureDesc texDesc(uint32_t w, uint32_t h, const char* name)
{
    RGTextureDesc d;
    d.width = w;
    d.height = h;
    d.format = RGFormat::RGBA8_UNORM;
    d.debugName = name;
    return d;
}

static bool testEmptyGraph()
{
    const std::string name = "empty graph compiles and executes as a no-op";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;

    // execute() before compile() must do nothing at all.
    g.execute(backend);
    if (backend.beginCount != 0 || backend.endCount != 0)
        return fail(name, "execute before compile touched the backend");

    if (!g.compile())
        return fail(name, "empty graph did not compile");
    if (!g.isCompiled())
        return fail(name, "isCompiled false after successful compile");
    const RGStats& s = g.getStats();
    if (s.pass_count != 0 || s.resource_count != 0 || s.barrier_count != 0 || s.culled_count != 0)
        return fail(name, "empty graph stats are not all zero");

    g.execute(backend);
    if (backend.beginCount != 1 || backend.endCount != 1)
        return fail(name, "empty execute did not bracket exactly one frame");
    if (!backend.created.empty() || !backend.destroyed.empty() || !backend.barriers.empty())
        return fail(name, "empty execute created resources or barriers");
    if (!execLog.empty())
        return fail(name, "empty execute ran a pass");
    return pass(name);
}

static bool testUnusedOutputCulled()
{
    const std::string name = "pass writing a never-read transient is culled";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle tmp;

    g.addPass("DeadWrite",
              [&](RGBuilder& b) {
                  tmp = b.createTexture(texDesc(64, 64, "tmp"));
                  b.write(tmp);
              },
              [&](RGContext&) { execLog.push_back("DeadWrite"); });

    if (!g.compile())
        return fail(name, "compile failed");
    if (g.getStats().pass_count != 1 || g.getStats().culled_count != 1)
        return fail(name, "expected pass_count=1 culled_count=1");

    g.execute(backend);
    if (!execLog.empty())
        return fail(name, "culled pass executed");
    if (!backend.created.empty() || g.getStats().transient_creates != 0)
        return fail(name, "culled pass transient was created");
    const RGResourceNode* res = g.getResource(tmp.handle);
    if (!res || res->firstWrite != UINT32_MAX)
        return fail(name, "culled resource firstWrite was not left at UINT32_MAX");
    return pass(name);
}

static bool testTransitiveDeadChainCulled()
{
    const std::string name = "dead chain A->B->C is culled transitively";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle t1, t2, t3;

    g.addPass("A", [&](RGBuilder& b) { t1 = b.createTexture(texDesc(16, 16, "t1")); b.write(t1); },
              [&](RGContext&) { execLog.push_back("A"); });
    g.addPass("B", [&](RGBuilder& b) { t2 = b.createTexture(texDesc(16, 16, "t2")); b.read(t1); b.write(t2); },
              [&](RGContext&) { execLog.push_back("B"); });
    g.addPass("C", [&](RGBuilder& b) { t3 = b.createTexture(texDesc(16, 16, "t3")); b.read(t2); b.write(t3); },
              [&](RGContext&) { execLog.push_back("C"); });

    if (!g.compile())
        return fail(name, "compile failed");
    if (g.getStats().culled_count != 3)
        return fail(name, "expected all 3 passes culled");
    g.execute(backend);
    if (!execLog.empty() || !backend.created.empty())
        return fail(name, "dead chain executed or allocated");
    return pass(name);
}

static bool testSideEffectSurvives()
{
    const std::string name = "side-effect pass is never culled even with unused output";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle tmp;

    g.addPass("PresentLike",
              [&](RGBuilder& b) {
                  tmp = b.createTexture(texDesc(32, 32, "tmp"));
                  b.write(tmp);
                  b.setSideEffect();
              },
              [&](RGContext&) { execLog.push_back("PresentLike"); });

    if (!g.compile())
        return fail(name, "compile failed");
    if (g.getStats().culled_count != 0)
        return fail(name, "side-effect pass was culled");
    g.execute(backend);
    if (execLog.size() != 1 || backend.created.size() != 1 || backend.destroyed.size() != 1)
        return fail(name, "side-effect pass did not execute with one transient");
    if (g.getStats().transient_creates != 1)
        return fail(name, "transient_creates != 1");
    return pass(name);
}

static bool testLiveChainOrderAndBarriers()
{
    const std::string name = "live chain executes in order with exact barriers";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle backbuffer = g.importTexture("bb", texDesc(128, 128, "bb"), RGResourceUsage::Present);
    RGTextureHandle tmp;

    g.addPass("Write", [&](RGBuilder& b) { tmp = b.createTexture(texDesc(128, 128, "tmp")); b.write(tmp, RGResourceUsage::RenderTarget); },
              [&](RGContext&) { execLog.push_back("Write"); });
    g.addPass("Blit",
              [&](RGBuilder& b) {
                  b.read(tmp, RGResourceUsage::ShaderResource);
                  b.write(backbuffer, RGResourceUsage::RenderTarget);
                  b.setSideEffect();
              },
              [&](RGContext&) { execLog.push_back("Blit"); });

    if (!g.compile())
        return fail(name, "compile failed");
    if (g.getStats().culled_count != 0 || g.getStats().barrier_count != 3)
        return fail(name, "expected culled=0 barrier_count=3");
    g.execute(backend);

    if (execLog.size() != 2 || execLog[0] != "Write" || execLog[1] != "Blit")
        return fail(name, "execution order is not Write,Blit");
    // Barriers: tmp Undefined->RenderTarget, tmp RenderTarget->ShaderResource,
    // backbuffer Present->RenderTarget (import usage honoured).
    if (backend.barriers.size() != 3)
        return fail(name, "backend did not receive exactly 3 barriers");
    const auto& b0 = backend.barriers[0];
    const auto& b1 = backend.barriers[1];
    const auto& b2 = backend.barriers[2];
    if (b0.handle.index != tmp.handle.index || b0.from != RGResourceUsage::Undefined || b0.to != RGResourceUsage::RenderTarget)
        return fail(name, "barrier 0 mismatch");
    if (b1.handle.index != tmp.handle.index || b1.from != RGResourceUsage::RenderTarget || b1.to != RGResourceUsage::ShaderResource)
        return fail(name, "barrier 1 mismatch");
    if (b2.handle.index != backbuffer.handle.index || b2.from != RGResourceUsage::Present || b2.to != RGResourceUsage::RenderTarget)
        return fail(name, "barrier 2 mismatch");
    if (backend.flushCount != 2)
        return fail(name, "expected one flush per pass that has barriers (2)");
    if (backend.created.size() != 1 || backend.created[0].index != tmp.handle.index)
        return fail(name, "imported backbuffer must not be created as transient");
    return pass(name);
}

static bool testDeclarationOrderWar()
{
    const std::string name = "read-before-write keeps declaration order (WAR)";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle src = g.importTexture("src", texDesc(16, 16, "src"), RGResourceUsage::ShaderResource);
    RGTextureHandle mid, outT;
    outT = g.importTexture("out", texDesc(16, 16, "out"), RGResourceUsage::Undefined);

    g.addPass("ReadFirst",
              [&](RGBuilder& b) { mid = b.createTexture(texDesc(16, 16, "mid")); b.read(src); b.write(mid); },
              [&](RGContext&) { execLog.push_back("ReadFirst"); });
    g.addPass("OverwriteSrc",
              [&](RGBuilder& b) { b.write(src, RGResourceUsage::RenderTarget); b.setSideEffect(); },
              [&](RGContext&) { execLog.push_back("OverwriteSrc"); });
    g.addPass("Consume",
              [&](RGBuilder& b) { b.read(mid); b.write(outT); b.setSideEffect(); },
              [&](RGContext&) { execLog.push_back("Consume"); });

    if (!g.compile())
        return fail(name, "compile failed");
    g.execute(backend);
    if (execLog.size() != 3 || execLog[0] != "ReadFirst" || execLog[1] != "OverwriteSrc" || execLog[2] != "Consume")
        return fail(name, "declaration order was not preserved");
    return pass(name);
}

static bool testMutualDependencyAsImplemented()
{
    const std::string name = "mutually dependent passes compile in declaration order";
    // The compiler only creates edges from earlier-declared to later-declared
    // passes, so a mutual dependency is not a cycle error: compilation
    // succeeds and execution follows declaration order. This pins the
    // behaviour as implemented in RGCompiler::topologicalSort.
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle x = g.importTexture("x", texDesc(16, 16, "x"), RGResourceUsage::ShaderResource);
    RGTextureHandle y = g.importTexture("y", texDesc(16, 16, "y"), RGResourceUsage::ShaderResource);

    g.addPass("A", [&](RGBuilder& b) { b.read(y); b.write(x, RGResourceUsage::RenderTarget); },
              [&](RGContext&) { execLog.push_back("A"); });
    g.addPass("B",
              [&](RGBuilder& b) { b.read(x); b.write(y, RGResourceUsage::RenderTarget); b.setSideEffect(); },
              [&](RGContext&) { execLog.push_back("B"); });

    if (!g.compile())
        return fail(name, "mutual dependency was rejected (expected success as implemented)");
    g.execute(backend);
    if (execLog.size() != 2 || execLog[0] != "A" || execLog[1] != "B")
        return fail(name, "mutual dependency did not execute in declaration order");
    return pass(name);
}

static bool testMissingResourceTolerated()
{
    const std::string name = "forged missing-resource handle is tolerated";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle forged;
    forged.handle.index = 999;
    forged.handle.version = 0;

    g.addPass("Reader",
              [&](RGBuilder& b) { b.read(forged, RGResourceUsage::ShaderResource); b.setSideEffect(); },
              [&](RGContext&) { execLog.push_back("Reader"); });

    if (!g.compile())
        return fail(name, "compile rejected a dangling handle");
    if (g.getResource(forged.handle) != nullptr)
        return fail(name, "getResource on missing index did not return nullptr");
    g.execute(backend);
    if (execLog.size() != 1)
        return fail(name, "pass reading a missing resource did not execute");
    if (!backend.created.empty())
        return fail(name, "missing resource was created as transient");
    if (backend.barriers.size() != 1 || backend.barriers[0].handle.index != 999 ||
        backend.barriers[0].from != RGResourceUsage::Undefined || backend.barriers[0].to != RGResourceUsage::ShaderResource)
        return fail(name, "expected one Undefined->ShaderResource barrier for handle 999");
    return pass(name);
}

static bool testLifetimes()
{
    const std::string name = "resource lifetimes firstWrite/lastRead are exact";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle imported = g.importTexture("in", texDesc(16, 16, "in"), RGResourceUsage::ShaderResource);
    RGTextureHandle t, u, w;

    g.addPass("P0", [&](RGBuilder& b) { t = b.createTexture(texDesc(16, 16, "t")); b.write(t); },
              [&](RGContext&) { execLog.push_back("P0"); });
    g.addPass("P1",
              [&](RGBuilder& b) { u = b.createTexture(texDesc(16, 16, "u")); w = b.createTexture(texDesc(16, 16, "w")); b.read(t); b.write(u); b.write(w); },
              [&](RGContext&) { execLog.push_back("P1"); });
    g.addPass("P2",
              [&](RGBuilder& b) { b.read(t); b.read(u); b.read(imported); b.setSideEffect(); },
              [&](RGContext&) { execLog.push_back("P2"); });

    if (!g.compile())
        return fail(name, "compile failed");
    const RGResourceNode* rt = g.getResource(t.handle);
    const RGResourceNode* ru = g.getResource(u.handle);
    const RGResourceNode* rw = g.getResource(w.handle);
    const RGResourceNode* ri = g.getResource(imported.handle);
    if (!rt || !ru || !rw || !ri)
        return fail(name, "getResource returned null for a live handle");
    if (rt->firstWrite != 0 || rt->lastRead != 2)
        return fail(name, "t lifetime is not [firstWrite=0,lastRead=2]");
    if (ru->firstWrite != 1 || ru->lastRead != 2)
        return fail(name, "u lifetime is not [firstWrite=1,lastRead=2]");
    // Write-only resource: firstWrite set, lastRead stays at its 0 default.
    if (rw->firstWrite != 1 || rw->lastRead != 0)
        return fail(name, "write-only w lifetime is not [firstWrite=1,lastRead=0]");
    // Imported read-only resource: never written by the graph.
    if (ri->firstWrite != UINT32_MAX || ri->lastRead != 2)
        return fail(name, "imported read-only lifetime is not [firstWrite=MAX,lastRead=2]");
    return pass(name);
}

static bool testTransientCounters()
{
    const std::string name = "transient create/destroy counters and reuse counter";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle a, b, dead;
    RGTextureHandle imported = g.importTexture("imp", texDesc(16, 16, "imp"), RGResourceUsage::ShaderResource);

    g.addPass("MakeA", [&](RGBuilder& x) { a = x.createTexture(texDesc(16, 16, "a")); x.write(a); },
              [&](RGContext&) { execLog.push_back("MakeA"); });
    g.addPass("MakeB", [&](RGBuilder& x) { b = x.createTexture(texDesc(16, 16, "b")); x.read(a); x.write(b); },
              [&](RGContext&) { execLog.push_back("MakeB"); });
    g.addPass("Dead", [&](RGBuilder& x) { dead = x.createTexture(texDesc(16, 16, "dead")); x.write(dead); },
              [&](RGContext&) { execLog.push_back("Dead"); });
    g.addPass("Final",
              [&](RGBuilder& x) { x.read(b); x.read(imported); x.setSideEffect(); },
              [&](RGContext&) { execLog.push_back("Final"); });

    if (!g.compile())
        return fail(name, "compile failed");
    if (g.getStats().culled_count != 1)
        return fail(name, "expected exactly the Dead pass culled");
    g.execute(backend);
    if (backend.beginCount != 1 || backend.endCount != 1)
        return fail(name, "frame brackets != 1");
    if (backend.created.size() != 2 || backend.destroyed.size() != 2)
        return fail(name, "expected 2 transient creates and 2 destroys");
    if (g.getStats().transient_creates != 2)
        return fail(name, "transient_creates != 2");
    // No aliasing pool is implemented, so reuse is always zero as implemented.
    if (g.getStats().transient_reuses != 0)
        return fail(name, "transient_reuses != 0 (no reuse implemented)");
    for (const auto& h : backend.created)
        if (h.index == imported.handle.index || h.index == dead.handle.index)
            return fail(name, "imported or culled resource was created");
    // A second execute re-creates and accumulates the counter until reset.
    g.execute(backend);
    if (g.getStats().transient_creates != 4 || backend.created.size() != 4)
        return fail(name, "second execute did not re-create transients (counter accumulates)");
    return pass(name);
}

static bool testRelativeDimensions()
{
    const std::string name = "relative dimensions resolve with ceil and 1x1 floor";
    RenderGraph g;
    g.setReferenceResolution(1919, 1079);
    RGTextureHandle half, absolute, tiny;
    g.addPass("Make",
              [&](RGBuilder& b) {
                  RGTextureDesc hd = texDesc(0, 0, "half");
                  hd.scaleFactor = 0.5f;
                  half = b.createTexture(hd);
                  absolute = b.createTexture(texDesc(64, 32, "abs"));
                  RGTextureDesc td = texDesc(0, 0, "tiny");
                  td.scaleFactor = 0.0001f;
                  tiny = b.createTexture(td);
                  b.write(half);
                  b.write(absolute);
                  b.write(tiny);
                  b.setSideEffect();
              },
              [](RGContext&) {});
    if (!g.compile())
        return fail(name, "compile failed");
    const RGResourceNode* rh = g.getResource(half.handle);
    const RGResourceNode* ra = g.getResource(absolute.handle);
    const RGResourceNode* rt = g.getResource(tiny.handle);
    if (!rh || !ra || !rt)
        return fail(name, "missing resource node");
    if (rh->desc.width != 960 || rh->desc.height != 540)
        return fail(name, "0.5x of 1919x1079 is not ceil to 960x540");
    if (ra->desc.width != 64 || ra->desc.height != 32)
        return fail(name, "absolute desc was modified");
    if (rt->desc.width != 1 || rt->desc.height != 1)
        return fail(name, "tiny relative desc was not floored to 1x1");

    // Without a reference resolution, relative descs stay unresolved.
    RenderGraph g2;
    RGTextureHandle rel;
    g2.addPass("Make",
               [&](RGBuilder& b) {
                   RGTextureDesc d = texDesc(7, 9, "rel");
                   d.scaleFactor = 0.5f;
                   rel = b.createTexture(d);
                   b.write(rel);
                   b.setSideEffect();
               },
               [](RGContext&) {});
    if (!g2.compile())
        return fail(name, "second compile failed");
    const RGResourceNode* rr = g2.getResource(rel.handle);
    if (!rr || rr->desc.width != 7 || rr->desc.height != 9)
        return fail(name, "relative desc changed without reference resolution");
    return pass(name);
}

static bool testSameUsageNoBarrier()
{
    const std::string name = "same-usage accesses emit no barrier, changes emit exactly one each";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle imp = g.importTexture("imp", texDesc(16, 16, "imp"), RGResourceUsage::ShaderResource);
    RGTextureHandle t;

    g.addPass("P0",
              [&](RGBuilder& b) { t = b.createTexture(texDesc(16, 16, "t")); b.read(imp, RGResourceUsage::ShaderResource); b.write(t, RGResourceUsage::RenderTarget); },
              [&](RGContext&) { execLog.push_back("P0"); });
    g.addPass("P1",
              [&](RGBuilder& b) { b.read(imp, RGResourceUsage::ShaderResource); b.read(t, RGResourceUsage::ShaderResource); b.setSideEffect(); },
              [&](RGContext&) { execLog.push_back("P1"); });

    if (!g.compile())
        return fail(name, "compile failed");
    if (g.getStats().barrier_count != 2)
        return fail(name, "expected exactly 2 barriers in stats");
    g.execute(backend);
    if (backend.barriers.size() != 2)
        return fail(name, "backend barrier count != 2");
    if (backend.barriers[0].handle.index != t.handle.index || backend.barriers[0].from != RGResourceUsage::Undefined ||
        backend.barriers[0].to != RGResourceUsage::RenderTarget)
        return fail(name, "barrier 0 is not t Undefined->RenderTarget");
    if (backend.barriers[1].handle.index != t.handle.index || backend.barriers[1].from != RGResourceUsage::RenderTarget ||
        backend.barriers[1].to != RGResourceUsage::ShaderResource)
        return fail(name, "barrier 1 is not t RenderTarget->ShaderResource");
    return pass(name);
}

static bool testReadWriteSingleBarrier()
{
    const std::string name = "readWrite with one usage emits a single barrier";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle imp = g.importTexture("imp", texDesc(16, 16, "imp"), RGResourceUsage::Undefined);

    g.addPass("RW",
              [&](RGBuilder& b) { b.readWrite(imp, RGResourceUsage::UnorderedAccess); b.setSideEffect(); },
              [&](RGContext&) { execLog.push_back("RW"); });

    if (!g.compile())
        return fail(name, "compile failed");
    g.execute(backend);
    if (backend.barriers.size() != 1 || backend.barriers[0].from != RGResourceUsage::Undefined ||
        backend.barriers[0].to != RGResourceUsage::UnorderedAccess)
        return fail(name, "expected exactly one Undefined->UnorderedAccess barrier");
    if (backend.flushCount != 1)
        return fail(name, "expected exactly one flush");
    return pass(name);
}

static bool testLargeChain()
{
    const std::string name = "512-pass chain compiles and executes in order";
    const int kCount = 512;
    RenderGraph g;
    FakeBackend backend;
    std::vector<int> order;
    RGTextureHandle prev;
    bool first = true;
    for (int i = 0; i < kCount; ++i)
    {
        g.addPass("Link",
                  [&, i](RGBuilder& b) {
                      RGTextureHandle cur = b.createTexture(texDesc(8, 8, "link"));
                      if (!first)
                          b.read(prev);
                      first = false;
                      b.write(cur);
                      prev = cur;
                      if (i == kCount - 1)
                          b.setSideEffect();
                  },
                  [&, i](RGContext&) { order.push_back(i); });
    }
    if (!g.compile())
        return fail(name, "large chain did not compile");
    if (g.getStats().pass_count != (uint64_t)kCount || g.getStats().culled_count != 0)
        return fail(name, "large chain stats mismatch");
    g.execute(backend);
    if ((int)order.size() != kCount)
        return fail(name, "not all large-chain passes executed");
    for (int i = 0; i < kCount; ++i)
        if (order[(size_t)i] != i)
            return fail(name, "large chain executed out of order");
    if (backend.created.size() != (size_t)kCount)
        return fail(name, "large chain transient count mismatch");
    return pass(name);
}

static bool testResetClears()
{
    const std::string name = "reset clears passes, resources, stats and compiled flag";
    RenderGraph g;
    FakeBackend backend;
    std::vector<std::string> execLog;
    RGTextureHandle t;
    g.addPass("One",
              [&](RGBuilder& b) { t = b.createTexture(texDesc(16, 16, "t")); b.write(t); b.setSideEffect(); },
              [&](RGContext&) { execLog.push_back("One"); });
    if (!g.compile())
        return fail(name, "initial compile failed");
    g.execute(backend);
    if (backend.created.size() != 1)
        return fail(name, "setup execute failed");

    g.reset();
    if (g.isCompiled())
        return fail(name, "isCompiled still true after reset");
    const RGStats& s = g.getStats();
    if (s.pass_count != 0 || s.resource_count != 0 || s.transient_creates != 0)
        return fail(name, "stats not cleared by reset");
    FakeBackend backend2;
    g.execute(backend2);
    if (backend2.beginCount != 0)
        return fail(name, "execute after reset without compile touched backend");
    if (!g.compile())
        return fail(name, "compile after reset failed");
    g.execute(backend2);
    if (backend2.beginCount != 1 || !backend2.created.empty())
        return fail(name, "reset graph is not empty");
    return pass(name);
}

int main()
{
    bool ok = true;
    ok &= testEmptyGraph();
    ok &= testUnusedOutputCulled();
    ok &= testTransitiveDeadChainCulled();
    ok &= testSideEffectSurvives();
    ok &= testLiveChainOrderAndBarriers();
    ok &= testDeclarationOrderWar();
    ok &= testMutualDependencyAsImplemented();
    ok &= testMissingResourceTolerated();
    ok &= testLifetimes();
    ok &= testTransientCounters();
    ok &= testRelativeDimensions();
    ok &= testSameUsageNoBarrier();
    ok &= testReadWriteSingleBarrier();
    ok &= testLargeChain();
    ok &= testResetClears();
    std::cout << (ok ? "all rendergraph tests passed" : "rendergraph tests FAILED") << std::endl;
    return ok ? 0 : 1;
}


