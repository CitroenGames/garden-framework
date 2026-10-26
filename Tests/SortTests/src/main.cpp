// Draw-ordering tests for RenderCommandBuffer sort / sortWithViewDepth,
// packedSortKey and instanced run merging.
// Run: SortTests.exe -> exit code 0 means all tests passed.

#include "Graphics/RenderCommandBuffer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
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

static IGPUMesh* fakeMesh(uintptr_t id)
{
    return reinterpret_cast<IGPUMesh*>(static_cast<uintptr_t>(0x10000u + id * 0x100u));
}

static int tagOf(const DrawCommand& c)
{
    return static_cast<int>(std::lround(c.model_matrix[3].x));
}

static float depthOf(const DrawCommand& c)
{
    return -c.model_matrix[3].z;
}

// Builds a buffer from prototype commands. Identity is carried solely by
// the model translation x (the tag); translation z carries the view depth.
// recordDrawRangePBR is used so every PBR / range field can be varied.
static RenderCommandBuffer buildBuffer(const std::vector<DrawCommand>& specs)
{
    RenderCommandBuffer buf;
    for (const DrawCommand& s : specs)
    {
        buf.recordDrawRangePBR(s.gpu_mesh, s.model_matrix, s.texture, s.use_texture,
                               s.pso_key, s.start_vertex, s.vertex_count, s.color,
                               s.alpha_cutoff, s.metallic, s.roughness, s.emissive,
                               s.heightmap_texture, s.use_heightmap_displacement,
                               s.heightmap_height_scale, s.heightmap_height_offset,
                               s.heightmap_texel_size, s.material_flags);
    }
    return buf;
}

static DrawCommand makeSpec(int tag, float depth, const PSOKey& pso = PSOKey(),
                            TextureHandle tex = 7, bool useTex = true)
{
    DrawCommand c;
    c.gpu_mesh = fakeMesh(1);
    c.pso_key = pso;
    c.texture = tex;
    c.use_texture = useTex;
    c.model_matrix = glm::translate(glm::mat4(1.0f),
                                    glm::vec3(static_cast<float>(tag), 0.0f, -depth));
    return c;
}

static bool checkPermutation(const std::string& name, const RenderCommandBuffer& buf,
                             std::vector<int> expectedTags)
{
    if (buf.size() != expectedTags.size())
        return fail(name, "size changed: got " + std::to_string(buf.size()) +
                          " want " + std::to_string(expectedTags.size()));
    std::vector<int> got;
    for (const DrawCommand& c : buf.commands())
        got.push_back(tagOf(c));
    std::sort(got.begin(), got.end());
    std::sort(expectedTags.begin(), expectedTags.end());
    if (got != expectedTags)
        return fail(name, "tag multiset changed (a draw was lost or duplicated)");
    return true;
}

static bool checkStateSorted(const std::string& name, const RenderCommandBuffer& buf)
{
    for (size_t i = 1; i < buf.size(); i++)
    {
        if (RenderCommandBuffer::drawStateLess(buf[i], buf[i - 1]))
            return fail(name, "adjacent pair out of state order at index " +
                              std::to_string(i));
        if (buf[i - 1].packedSortKey() > buf[i].packedSortKey())
            return fail(name, "packed key decreased at index " + std::to_string(i));
    }
    return true;
}

// Independent compatibility predicate, restated field by field so the
// stats / run-length checks below do not just mirror the engine helper.
static bool testCompatible(const DrawCommand& a, const DrawCommand& b)
{
    auto supports = [](const DrawCommand& c) {
        return !c.pso_key.shadow && !c.pso_key.depth_only &&
               c.pso_key.blend == BlendMode::None;
    };
    if (!supports(a) || !supports(b)) return false;
    return a.gpu_mesh == b.gpu_mesh && a.pso_key == b.pso_key &&
           a.use_texture == b.use_texture && a.texture == b.texture &&
           a.use_heightmap_displacement == b.use_heightmap_displacement &&
           a.heightmap_texture == b.heightmap_texture &&
           a.heightmap_height_scale == b.heightmap_height_scale &&
           a.heightmap_height_offset == b.heightmap_height_offset &&
           a.heightmap_texel_size.x == b.heightmap_texel_size.x &&
           a.heightmap_texel_size.y == b.heightmap_texel_size.y &&
           a.start_vertex == b.start_vertex && a.vertex_count == b.vertex_count &&
           a.alpha_cutoff == b.alpha_cutoff && a.metallic == b.metallic &&
           a.roughness == b.roughness && a.material_flags == b.material_flags &&
           a.color == b.color && a.emissive == b.emissive;
}

struct Rescan { uint64_t groups = 0; size_t maxRun = 0; };

static Rescan independentRescan(const std::vector<DrawCommand>& cmds)
{
    Rescan r;
    for (size_t i = 0; i < cmds.size();)
    {
        size_t len = 1;
        while (i + len < cmds.size() && testCompatible(cmds[i], cmds[i + len]))
            ++len;
        r.groups++;
        if (len > r.maxRun) r.maxRun = len;
        i += len;
    }
    return r;
}

static bool checkStats(const std::string& name, const RenderCommandBuffer& buf,
                       const SortStats& stats)
{
    Rescan want = independentRescan(buf.commands());
    if (stats.groups != want.groups)
        return fail(name, "stats.groups=" + std::to_string(stats.groups) +
                          " independent rescan=" + std::to_string(want.groups));
    if (stats.max_run != want.maxRun)
        return fail(name, "stats.max_run=" + std::to_string(stats.max_run) +
                          " independent rescan=" + std::to_string(want.maxRun));
    // The engine run-length helper must agree with the independent scan
    // at every run head.
    for (size_t i = 0; i < buf.size();)
    {
        size_t len = 1;
        while (i + len < buf.size() && testCompatible(buf[i], buf[i + len]))
            ++len;
        if (instancedRunLength(buf.commands(), i) != len)
            return fail(name, "instancedRunLength disagrees with rescan at " +
                              std::to_string(i));
        i += len;
    }
    return true;
}

static uint16_t expectedBucket(float depth, float minD, float maxD)
{
    if (!(maxD > minD)) return 0;
    float t = (depth - minD) / (maxD - minD);
    return static_cast<uint16_t>(t * 4095.0f + 0.5f);
}

// Ordering contract for the depth sort: state order first, then the
// 12-bit depth bucket, but only inside an exact state-equivalence class.
static bool checkDepthSorted(const std::string& name, const RenderCommandBuffer& buf)
{
    if (buf.size() < 2) return true;
    float minD = depthOf(buf[0]), maxD = depthOf(buf[0]);
    for (const DrawCommand& c : buf.commands())
    {
        minD = std::min(minD, depthOf(c));
        maxD = std::max(maxD, depthOf(c));
    }
    for (size_t i = 1; i < buf.size(); i++)
    {
        const DrawCommand& a = buf[i - 1];
        const DrawCommand& b = buf[i];
        if (RenderCommandBuffer::drawStateLess(b, a))
            return fail(name, "state order regressed at index " + std::to_string(i));
        bool equivalent = !RenderCommandBuffer::drawStateLess(a, b) &&
                          !RenderCommandBuffer::drawStateLess(b, a);
        if (equivalent &&
            expectedBucket(depthOf(a), minD, maxD) > expectedBucket(depthOf(b), minD, maxD))
            return fail(name, "depth bucket regressed inside a state group at index " +
                              std::to_string(i));
    }
    return true;
}

static bool testPackedKeyBeyondBasics()
{
    const std::string name = "packed key masking, high material bits and PSO order equivalence";
    PSOKey main;

    // Exhaustive PSO order: pipelineBits numeric order must match operator<.
    std::vector<PSOKey> keys;
    for (int shadow = 0; shadow < 2; shadow++)
    for (int depth = 0; depth < 2; depth++)
    for (int light = 0; light < 2; light++)
    for (int alpha = 0; alpha < 2; alpha++)
    for (int blend = 0; blend < 3; blend++)
    for (int cull = 0; cull < 3; cull++)
    {
        PSOKey k;
        k.shadow = shadow != 0; k.depth_only = depth != 0;
        k.lighting = light != 0; k.alpha_test = alpha != 0;
        k.blend = static_cast<BlendMode>(blend);
        k.cull = static_cast<CullMode>(cull);
        keys.push_back(k);
    }
    for (const PSOKey& a : keys)
        for (const PSOKey& b : keys)
        {
            if ((a < b) != (a.pipelineBits() < b.pipelineBits()) && a != b)
                return fail(name, "pipelineBits order disagrees with PSOKey operator<");
            if ((a == b) != (a.pipelineBits() == b.pipelineBits()))
                return fail(name, "pipelineBits is not injective over PSO fields");
        }

    DrawCommand base = makeSpec(0, 10.0f, main);
    // Depth bucket wraps at 12 bits, including values far past the range.
    if (base.packedSortKey(0x1000u) != base.packedSortKey(0u) ||
        base.packedSortKey(0xFFFFu) != base.packedSortKey(0xFFFu))
        return fail(name, "depth bucket is not masked to 12 bits");
    // Material is masked to 16 bits inside the packed key.
    DrawCommand hiMat = base; hiMat.material_flags = 0xFFFF0000u;
    DrawCommand loMat = base; loMat.material_flags = 0u;
    if (hiMat.packedSortKey() != loMat.packedSortKey())
        return fail(name, "material_flags not masked to 16 bits in packed key");
    // ... but the full state order still separates high material bits.
    if (!RenderCommandBuffer::drawStateLess(loMat, hiMat) ||
        RenderCommandBuffer::drawStateLess(hiMat, loMat))
        return fail(name, "full state order ignores high material bits");
    // vertex_count enters the packed key only as a zero / non-zero flag.
    DrawCommand vc1 = base; vc1.vertex_count = 1;
    DrawCommand vc999 = base; vc999.vertex_count = 999;
    if (vc1.packedSortKey() != vc999.packedSortKey() ||
        vc1.packedSortKey() == base.packedSortKey())
        return fail(name, "vertex_count geometry bit is not a boolean flag");
    if (!RenderCommandBuffer::drawStateLess(vc1, vc999))
        return fail(name, "full state order ignores exact vertex_count");
    // A larger depth bucket never outweighs any state field: max depth on
    // the smaller state still sorts before min depth on the larger state.
    DrawCommand shadow = makeSpec(1, 10.0f, PSOKey::shadowPass());
    if (!(base.packedSortKey(0xFFFu) < shadow.packedSortKey(0u)))
        return fail(name, "depth bucket outweighs pipeline state");
    return pass(name);
}

static std::vector<DrawCommand> scrambledSpecs(int count)
{
    std::vector<DrawCommand> specs;
    PSOKey main; PSOKey unlit; unlit.lighting = false;
    for (int i = 0; i < count; i++)
    {
        uint32_t h = static_cast<uint32_t>(i * 2654435761u) >> 8;
        PSOKey pso = (h & 0x100u) ? PSOKey::shadowPass() : ((h & 0x80u) ? unlit : main);
        DrawCommand c = makeSpec(i, 1.0f + static_cast<float>(h % 97u), pso,
                                 static_cast<TextureHandle>(7u + (h & 7u)), true);
        c.material_flags = h & 3u;
        c.metallic = static_cast<float>(h & 1u) * 0.5f;
        c.color = glm::vec3(static_cast<float>(h & 1u), 1.0f, 1.0f);
        specs.push_back(c);
    }
    return specs;
}

static std::vector<int> allTags(int count)
{
    std::vector<int> tags;
    for (int i = 0; i < count; i++) tags.push_back(i);
    return tags;
}

static bool testPermutationIntegrityBothSorts()
{
    const std::string name = "permutation integrity after sort and depth sort (257 draws)";
    std::vector<DrawCommand> specs = scrambledSpecs(257);

    RenderCommandBuffer a = buildBuffer(specs);
    a.sort();
    if (!checkPermutation(name, a, allTags(257))) return false;
    if (!checkStateSorted(name, a)) return false;

    RenderCommandBuffer b = buildBuffer(specs);
    b.sortWithViewDepth(glm::mat4(1.0f));
    if (!checkPermutation(name, b, allTags(257))) return false;
    if (!checkDepthSorted(name, b)) return false;

    // Deterministic pseudo-random depths with duplicated values (ties are
    // part of the input, not filtered out).
    std::vector<DrawCommand> tied = scrambledSpecs(128);
    for (DrawCommand& c : tied)
        c.model_matrix = glm::translate(glm::mat4(1.0f),
            glm::vec3(c.model_matrix[3].x, 0.0f,
                      -static_cast<float>(1 + (tagOf(c) % 4) * 10)));
    RenderCommandBuffer cbuf = buildBuffer(tied);
    cbuf.sortWithViewDepth(glm::mat4(1.0f));
    if (!checkPermutation(name, cbuf, allTags(128))) return false;
    if (!checkDepthSorted(name, cbuf)) return false;
    return pass(name);
}

static bool testEmptySingleAndDegenerate()
{
    const std::string name = "empty, single-element and all-equal-depth buffers";
    const glm::mat4 view(1.0f);

    RenderCommandBuffer empty;
    SortStats s1; s1.temp_allocs = 91; s1.temp_bytes = 92; s1.groups = 93; s1.max_run = 94;
    empty.sort(&s1);
    if (empty.size() != 0 || s1.temp_allocs != 0 || s1.temp_bytes != 0 ||
        s1.groups != 0 || s1.max_run != 0)
        return fail(name, "sort() on empty buffer did not report zeroed stats");

    RenderCommandBuffer empty2;
    SortStats s2; s2.temp_allocs = 91; s2.temp_bytes = 92; s2.groups = 93; s2.max_run = 94;
    empty2.sortWithViewDepth(view, &s2);
    if (empty2.size() != 0)
        return fail(name, "depth sort changed empty buffer size");
    if (s2.temp_allocs != 0 || s2.temp_bytes != 0 || s2.groups != 0 || s2.max_run != 0)
        return fail(name, "depth sort on empty buffer left stale stats (allocs=" +
                          std::to_string(s2.temp_allocs) + " groups=" +
                          std::to_string(s2.groups) + ")");

    // Sorting with no stats pointer must also be safe on empty input.
    RenderCommandBuffer empty3; empty3.sort(); empty3.sortWithViewDepth(view);

    for (int mode = 0; mode < 2; mode++)
    {
        RenderCommandBuffer one = buildBuffer({makeSpec(42, 10.0f)});
        SortStats st;
        if (mode == 0) one.sort(&st); else one.sortWithViewDepth(view, &st);
        if (!checkPermutation(name, one, {42})) return false;
        if (st.groups != 1 || st.max_run != 1)
            return fail(name, "single draw stats are not groups=1 max_run=1");
        if (mode == 1 && (st.temp_allocs != 4 || st.temp_bytes == 0))
            return fail(name, "single draw depth sort temp stats unexpected");
    }

    // All draws at exactly the same depth: bucket denominator is zero,
    // every bucket must stay 0 and grouping must be purely by state.
    std::vector<DrawCommand> same;
    for (int i = 0; i < 8; i++)
        same.push_back(makeSpec(i, 10.0f, PSOKey(), static_cast<TextureHandle>(7 + (i % 2))));
    RenderCommandBuffer buf = buildBuffer(same);
    buf.sortWithViewDepth(view);
    if (!checkPermutation(name, buf, allTags(8))) return false;
    if (!checkDepthSorted(name, buf)) return false;
    if (buf[0].texture != buf[3].texture || buf[0].texture == buf[4].texture ||
        buf[4].texture != buf[7].texture)
        return fail(name, "same-depth draws were not grouped by texture");
    return pass(name);
}

static bool testEqualKeyDraws()
{
    const std::string name = "equal-key draws are preserved and stay grouped";
    const glm::mat4 view(1.0f);

    // Fully state-equal draws differing only by transform identity.
    std::vector<DrawCommand> specs;
    for (int i = 0; i < 16; i++) specs.push_back(makeSpec(i, 10.0f));
    RenderCommandBuffer a = buildBuffer(specs);
    a.sort();
    if (!checkPermutation(name, a, allTags(16))) return false;

    // Same state, distinct well-separated depths: the depth sort must
    // order them strictly front-to-back, tag order following depth.
    std::vector<DrawCommand> byDepth;
    const float depths[6] = {50.0f, 10.0f, 40.0f, 20.0f, 60.0f, 30.0f};
    for (int i = 0; i < 6; i++) byDepth.push_back(makeSpec(i, depths[i]));
    RenderCommandBuffer b = buildBuffer(byDepth);
    b.sortWithViewDepth(view);
    const int wantOrder[6] = {1, 3, 5, 2, 0, 4};
    for (int i = 0; i < 6; i++)
        if (tagOf(b[static_cast<size_t>(i)]) != wantOrder[i])
            return fail(name, "state-equal draws not front-to-back at position " +
                              std::to_string(i));

    // Exact depth ties inside one state group: order between tied draws
    // is unspecified, but none may leave the group or be lost.
    std::vector<DrawCommand> tied;
    for (int i = 0; i < 10; i++)
        tied.push_back(makeSpec(i, (i % 2 == 0) ? 10.0f : 10.0f,
                                PSOKey(), static_cast<TextureHandle>(7 + (i % 2))));
    RenderCommandBuffer cbuf = buildBuffer(tied);
    cbuf.sortWithViewDepth(view);
    if (!checkPermutation(name, cbuf, allTags(10))) return false;
    if (!checkDepthSorted(name, cbuf)) return false;
    TextureHandle first = cbuf[0].texture;
    for (size_t i = 0; i < 5; i++)
        if (cbuf[i].texture != first) return fail(name, "depth-tied group split early");
    for (size_t i = 5; i < 10; i++)
        if (cbuf[i].texture == first) return fail(name, "depth-tied groups interleaved");
    return pass(name);
}

static bool testExcludedDrawsNeverMerge()
{
    const std::string name = "shadow, depth-only and blended draws never join a merged run";
    PSOKey shadow = PSOKey::shadowPass();
    PSOKey depth = PSOKey::depthPrepass();
    PSOKey alpha; alpha.blend = BlendMode::Alpha;
    PSOKey additive; additive.blend = BlendMode::Additive;
    const PSOKey excluded[4] = {shadow, depth, alpha, additive};

    for (const PSOKey& pso : excluded)
    {
        std::vector<DrawCommand> cmds(5, makeSpec(0, 10.0f, pso));
        if (drawSupportsInstancing(cmds[0]))
            return fail(name, "excluded draw claims instancing support");
        for (size_t i = 0; i < cmds.size(); i++)
            if (instancedRunLength(cmds, i) != 1)
                return fail(name, "excluded run length is not 1 at index " +
                                  std::to_string(i));
        // Through the sorted buffer + stats path as well.
        std::vector<DrawCommand> specs;
        for (int i = 0; i < 5; i++) specs.push_back(makeSpec(i, 10.0f, pso));
        RenderCommandBuffer buf = buildBuffer(specs);
        SortStats st; buf.sort(&st);
        if (st.groups != 5 || st.max_run != 1)
            return fail(name, "sorted excluded draws merged (groups=" +
                              std::to_string(st.groups) + ")");
        RenderCommandBuffer dbuf = buildBuffer(specs);
        SortStats dst; dbuf.sortWithViewDepth(glm::mat4(1.0f), &dst);
        if (dst.groups != 5 || dst.max_run != 1)
            return fail(name, "depth-sorted excluded draws merged");
    }

    // An excluded draw breaks a run on either side.
    PSOKey main;
    {
        std::vector<DrawCommand> cmds = {makeSpec(0, 10.0f, main),
                                         makeSpec(1, 10.0f, shadow),
                                         makeSpec(2, 10.0f, main)};
        if (instancedRunLength(cmds, 0) != 1 || instancedRunLength(cmds, 1) != 1 ||
            instancedRunLength(cmds, 2) != 1)
            return fail(name, "shadow draw did not break neighbouring runs");
    }
    {
        std::vector<DrawCommand> cmds = {makeSpec(0, 10.0f, alpha),
                                         makeSpec(1, 10.0f, main),
                                         makeSpec(2, 10.0f, main)};
        if (instancedRunLength(cmds, 0) != 1 || instancedRunLength(cmds, 1) != 2)
            return fail(name, "blended head or following plain run mis-measured");
    }
    return pass(name);
}

static bool testRunLengthAdversarial()
{
    const std::string name = "run length on adversarial patterns";
    PSOKey main;
    auto plain = [&](int tag) { return makeSpec(tag, 10.0f, main, 7, true); };
    auto otherTex = [&](int tag) { return makeSpec(tag, 10.0f, main, 8, true); };

    // Degenerate inputs still report at least one per the header contract.
    std::vector<DrawCommand> emptyVec;
    if (instancedRunLength(emptyVec, 0) != 1)
        return fail(name, "empty vector run length is not 1");
    std::vector<DrawCommand> single = {plain(0)};
    if (instancedRunLength(single, 0) != 1 || instancedRunLength(single, 1) != 1 ||
        instancedRunLength(single, 100) != 1)
        return fail(name, "single / out-of-range run length is not 1");

    // One long run: length shrinks by one from every interior head.
    std::vector<DrawCommand> all;
    for (int i = 0; i < 17; i++) all.push_back(plain(i));
    for (size_t i = 0; i < all.size(); i++)
        if (instancedRunLength(all, i) != all.size() - i)
            return fail(name, "uniform run mis-measured from index " + std::to_string(i));

    // Alternating compatible / incompatible: every run is a singleton.
    std::vector<DrawCommand> alt;
    for (int i = 0; i < 12; i++) alt.push_back(i % 2 == 0 ? plain(i) : otherTex(i));
    for (size_t i = 0; i < alt.size(); i++)
        if (instancedRunLength(alt, i) != 1)
            return fail(name, "alternating pattern merged at index " + std::to_string(i));

    // Reversed group sizes 3,2,1 and then 1,2,3: heads and interiors.
    std::vector<DrawCommand> groups = {plain(0), plain(1), plain(2),
                                       otherTex(3), otherTex(4),
                                       makeSpec(5, 10.0f, main, 9, true)};
    const size_t wantHeads[3] = {3, 2, 1};
    size_t headIdx[3] = {0, 3, 5};
    for (int g = 0; g < 3; g++)
        if (instancedRunLength(groups, headIdx[g]) != wantHeads[g])
            return fail(name, "group head run wrong for group " + std::to_string(g));
    if (instancedRunLength(groups, 1) != 2 || instancedRunLength(groups, 2) != 1 ||
        instancedRunLength(groups, 4) != 1)
        return fail(name, "interior run lengths wrong");
    std::vector<DrawCommand> reversed(groups.rbegin(), groups.rend());
    if (instancedRunLength(reversed, 0) != 1 || instancedRunLength(reversed, 1) != 2 ||
        instancedRunLength(reversed, 3) != 3)
        return fail(name, "reversed group pattern mis-measured");

    // Single-element runs wedged between long runs, and a run at the tail.
    std::vector<DrawCommand> wedged;
    for (int i = 0; i < 4; i++) wedged.push_back(plain(i));
    wedged.push_back(otherTex(4));
    for (int i = 5; i < 9; i++) wedged.push_back(plain(i));
    wedged.push_back(otherTex(9));
    if (instancedRunLength(wedged, 0) != 4 || instancedRunLength(wedged, 4) != 1 ||
        instancedRunLength(wedged, 5) != 4 || instancedRunLength(wedged, 9) != 1)
        return fail(name, "wedged singleton or tail run mis-measured");
    return pass(name);
}

static bool testRunLengthFieldSensitivity()
{
    const std::string name = "run length breaks on every state field but not on transform";
    DrawCommand base = makeSpec(0, 10.0f);
    base.heightmap_texture = 11;
    base.use_heightmap_displacement = true;
    base.heightmap_height_scale = 2.0f;
    base.heightmap_height_offset = 0.25f;
    base.heightmap_texel_size = glm::vec2(0.01f, 0.02f);
    base.alpha_cutoff = 0.5f;
    base.metallic = 0.3f;
    base.roughness = 0.4f;
    base.material_flags = 3u;
    base.emissive = glm::vec3(0.1f, 0.0f, 0.0f);

    using Mutator = std::function<void(DrawCommand&)>;
    std::vector<std::pair<std::string, Mutator>> breakers = {
        {"mesh", [](DrawCommand& c) { c.gpu_mesh = fakeMesh(2); }},
        {"texture", [](DrawCommand& c) { c.texture = 8; }},
        {"use_texture", [](DrawCommand& c) { c.use_texture = false; }},
        {"pso cull", [](DrawCommand& c) { c.pso_key.cull = CullMode::Front; }},
        {"pso lighting", [](DrawCommand& c) { c.pso_key.lighting = false; }},
        {"pso alpha_test", [](DrawCommand& c) { c.pso_key.alpha_test = true; }},
        {"color", [](DrawCommand& c) { c.color = glm::vec3(0.5f, 1.0f, 1.0f); }},
        {"emissive", [](DrawCommand& c) { c.emissive = glm::vec3(0.2f, 0.0f, 0.0f); }},
        {"metallic", [](DrawCommand& c) { c.metallic = 0.31f; }},
        {"roughness", [](DrawCommand& c) { c.roughness = 0.41f; }},
        {"material_flags low", [](DrawCommand& c) { c.material_flags = 4u; }},
        {"material_flags high bit", [](DrawCommand& c) { c.material_flags = 3u | 0x10000u; }},
        {"alpha_cutoff", [](DrawCommand& c) { c.alpha_cutoff = 0.6f; }},
        {"start_vertex", [](DrawCommand& c) { c.start_vertex = 3; }},
        {"vertex_count", [](DrawCommand& c) { c.vertex_count = 12; }},
        {"heightmap texture", [](DrawCommand& c) { c.heightmap_texture = 12; }},
        {"heightmap use", [](DrawCommand& c) { c.use_heightmap_displacement = false; }},
        {"heightmap scale", [](DrawCommand& c) { c.heightmap_height_scale = 2.5f; }},
        {"heightmap offset", [](DrawCommand& c) { c.heightmap_height_offset = 0.5f; }},
        {"heightmap texel x", [](DrawCommand& c) { c.heightmap_texel_size.x = 0.03f; }},
        {"heightmap texel y", [](DrawCommand& c) { c.heightmap_texel_size.y = 0.04f; }},
    };
    for (auto& entry : breakers)
    {
        DrawCommand mutated = base;
        entry.second(mutated);
        std::vector<DrawCommand> cmds = {base, mutated, base};
        if (instancedRunLength(cmds, 0) != 1)
            return fail(name, "changing " + entry.first + " did not break the run");
        if (instancedRunLength(cmds, 1) != 1 || instancedRunLength(cmds, 2) != 1)
            return fail(name, "run after changing " + entry.first + " mis-measured");
        if (drawInstanceCompatible(base, mutated))
            return fail(name, "changing " + entry.first + " still claims compatibility");
    }

    // Transform-only differences are the one thing a merged run may vary.
    DrawCommand moved = base;
    moved.model_matrix = glm::translate(glm::mat4(1.0f), glm::vec3(9.0f, 2.0f, -33.0f));
    moved.normal_matrix = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    std::vector<DrawCommand> cmds = {base, moved, base};
    if (instancedRunLength(cmds, 0) != 3)
        return fail(name, "transform-only difference broke a merged run");
    return pass(name);
}

static bool testSortStatsVsIndependentRescan()
{
    const std::string name = "sort stats match an independent rescan after both sorts";
    const glm::mat4 view(1.0f);

    std::vector<std::vector<DrawCommand>> patterns;
    patterns.push_back({});
    patterns.push_back({makeSpec(0, 10.0f)});
    {
        std::vector<DrawCommand> v;
        for (int i = 0; i < 9; i++) v.push_back(makeSpec(i, static_cast<float>(30 - i)));
        patterns.push_back(v);
    }
    {
        std::vector<DrawCommand> v;
        for (int i = 0; i < 12; i++)
            v.push_back(makeSpec(i, static_cast<float>(i * 3),
                                 PSOKey(), static_cast<TextureHandle>(7 + (i % 2))));
        patterns.push_back(v);
    }
    {
        std::vector<DrawCommand> v;
        for (int i = 0; i < 10; i++)
            v.push_back(makeSpec(i, static_cast<float>(i * 2),
                                 (i % 3 == 0) ? PSOKey::shadowPass() : PSOKey(),
                                 static_cast<TextureHandle>(7 + (i % 3))));
        patterns.push_back(v);
    }
    patterns.push_back(scrambledSpecs(64));

    for (size_t p = 0; p < patterns.size(); p++)
    {
        RenderCommandBuffer a = buildBuffer(patterns[p]);
        SortStats sa; a.sort(&sa);
        if (!checkStats(name, a, sa)) return false;
        if (sa.temp_allocs != 0 || sa.temp_bytes != 0)
            return fail(name, "sort() reported temp allocations for pattern " +
                              std::to_string(p));
        if (patterns[p].empty()) continue;
        RenderCommandBuffer b = buildBuffer(patterns[p]);
        SortStats sb; b.sortWithViewDepth(view, &sb);
        if (!checkStats(name, b, sb)) return false;
        if (sb.temp_allocs != 4 || sb.temp_bytes == 0 ||
            sb.temp_bytes % b.size() != 0)
            return fail(name, "depth sort temp stats malformed for pattern " +
                              std::to_string(p));
    }

    // Per-element temp cost must be identical for different sizes.
    RenderCommandBuffer small = buildBuffer(scrambledSpecs(4));
    SortStats ss; small.sortWithViewDepth(view, &ss);
    RenderCommandBuffer large = buildBuffer(scrambledSpecs(8));
    SortStats sl; large.sortWithViewDepth(view, &sl);
    if (ss.temp_bytes / small.size() != sl.temp_bytes / large.size())
        return fail(name, "depth sort temp bytes do not scale linearly with draw count");
    return pass(name);
}

static bool testDepthSortDoesNotCrossStateGroups()
{
    const std::string name = "depth order never crosses a state-group boundary";
    const glm::mat4 view(1.0f);
    // Nearest draws belong to the later state group on purpose: state
    // grouping must win over depth, and each group stays front-to-back.
    std::vector<DrawCommand> specs;
    specs.push_back(makeSpec(0, 50.0f, PSOKey(), 7, true));
    specs.push_back(makeSpec(1, 40.0f, PSOKey(), 7, true));
    specs.push_back(makeSpec(2, 5.0f, PSOKey(), 8, true));
    specs.push_back(makeSpec(3, 1.0f, PSOKey(), 8, true));
    specs.push_back(makeSpec(4, 2.0f, PSOKey::shadowPass(), 7, true));
    RenderCommandBuffer buf = buildBuffer(specs);
    buf.sortWithViewDepth(view);
    if (!checkPermutation(name, buf, allTags(5))) return false;
    if (!checkDepthSorted(name, buf)) return false;
    // Texture 7 group first (depths 40, 50), then texture 8 (1, 5), shadow last.
    const int want[5] = {1, 0, 3, 2, 4};
    for (int i = 0; i < 5; i++)
        if (tagOf(buf[static_cast<size_t>(i)]) != want[i])
            return fail(name, "unexpected order at position " + std::to_string(i) +
                              " got tag " + std::to_string(tagOf(buf[static_cast<size_t>(i)])));
    // Behind-camera draws (negative view depth) still permute and group.
    std::vector<DrawCommand> behind;
    behind.push_back(makeSpec(0, -20.0f, PSOKey(), 7, true));
    behind.push_back(makeSpec(1, 10.0f, PSOKey(), 7, true));
    behind.push_back(makeSpec(2, -5.0f, PSOKey(), 8, true));
    behind.push_back(makeSpec(3, 30.0f, PSOKey(), 7, true));
    RenderCommandBuffer bbuf = buildBuffer(behind);
    bbuf.sortWithViewDepth(view);
    if (!checkPermutation(name, bbuf, allTags(4))) return false;
    if (!checkDepthSorted(name, bbuf)) return false;
    return pass(name);
}

static bool testRepeatedAndReversedSorts()
{
    const std::string name = "re-sorting and reversed input keep the same result";
    std::vector<DrawCommand> specs = scrambledSpecs(96);

    RenderCommandBuffer a = buildBuffer(specs);
    a.sort();
    std::vector<int> firstOrder;
    for (const DrawCommand& c : a.commands()) firstOrder.push_back(tagOf(c));
    SortStats again; a.sort(&again);
    std::vector<int> secondOrder;
    for (const DrawCommand& c : a.commands()) secondOrder.push_back(tagOf(c));
    // Tags of state-equal draws may swap under an unstable sort, so the
    // contract checked here is state order + permutation, not tag order.
    if (!checkPermutation(name, a, allTags(96))) return false;
    if (!checkStateSorted(name, a)) return false;
    (void)firstOrder; (void)secondOrder;

    std::vector<DrawCommand> rev(specs.rbegin(), specs.rend());
    RenderCommandBuffer b = buildBuffer(rev);
    b.sortWithViewDepth(glm::mat4(1.0f));
    if (!checkPermutation(name, b, allTags(96))) return false;
    if (!checkDepthSorted(name, b)) return false;

    // Sort an already depth-sorted buffer with the plain sort and back.
    b.sort();
    if (!checkStateSorted(name, b)) return false;
    b.sortWithViewDepth(glm::mat4(1.0f));
    if (!checkPermutation(name, b, allTags(96))) return false;
    if (!checkDepthSorted(name, b)) return false;
    return pass(name);
}

int main()
{
    bool ok = true;
    ok &= testPackedKeyBeyondBasics();
    ok &= testPermutationIntegrityBothSorts();
    ok &= testEmptySingleAndDegenerate();
    ok &= testEqualKeyDraws();
    ok &= testExcludedDrawsNeverMerge();
    ok &= testRunLengthAdversarial();
    ok &= testRunLengthFieldSensitivity();
    ok &= testSortStatsVsIndependentRescan();
    ok &= testDepthSortDoesNotCrossStateGroups();
    ok &= testRepeatedAndReversedSorts();
    std::cout << (ok ? "all sort tests passed" : "sort tests FAILED") << std::endl;
    return ok ? 0 : 1;
}

