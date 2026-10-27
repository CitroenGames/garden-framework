#pragma once
#include "Audio/AudioSystem.hpp"
#include "Audio/SoundEvent.hpp"
#include "Audio/AudioSourceSystem.hpp"
#include "Components/AudioSourceComponent.hpp"
#include "Components/IdentityComponent.hpp"
#include "ECS/Hierarchy.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

namespace
{
struct MockAudioBackend
{
    SoundHandle next = 1;
    int play_calls = 0;
    std::vector<std::string> clips;
    std::vector<glm::vec3> positions;
    std::vector<SoundHandle> stopped;
    AudioEventSystem::PlayFn playFn()
    {
        return [this](const SoundEventDef&, const std::string& clip, const glm::vec3& position, bool, float, float) {
            ++play_calls; clips.push_back(clip); positions.push_back(position); return next++;
        };
    }
    AudioEventSystem::StopFn stopFn()
    {
        return [this](SoundHandle h) { stopped.push_back(h); };
    }
};
SoundEventDef makeTestDef(const std::string& name, int priority = 50, int max_voices = 4)
{
    SoundEventDef d; d.name = name; d.clip_paths = {"a.wav", "b.wav", "c.wav"};
    d.priority = priority; d.max_voices = max_voices; d.max_distance = 40.0f; return d;
}

bool writeSilentTestWav(const std::filesystem::path& path)
{
    // PCM WAV: 44.1 kHz mono, two silent samples.
    static constexpr std::array<unsigned char, 48> wav = {
        'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
        0x44, 0xAC, 0, 0, 0x88, 0x58, 1, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 4, 0, 0, 0, 0, 0, 0, 0,
    };
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    return file.good();
}
}

bool testAudioUnknownEventNoVoices()
{
    const std::string name = "Audio.EventsUnknownNoCrash";
    AudioEventSystem sys; MockAudioBackend mock;
    sys.setPlayFn(mock.playFn()); sys.setStopFn(mock.stopFn()); sys.setSeed(1);
    for (int i = 0; i < 1000; ++i)
        if (sys.playAt("no.such.event", glm::vec3(0)) != INVALID_SOUND) return fail(name, "unknown returned a handle");
    if (sys.activeVoiceCount() != 0 || mock.play_calls != 0) return fail(name, "unknown started voices");
    if (sys.diagnosticLogCount() > 1) return fail(name, "unknown log not rate-limited");
    if (sys.findEvent("no.such.event") != nullptr) return fail(name, "unknown lookup hit");
    return pass(name);
}

bool testAudioVoiceStealingSelection()
{
    const std::string name = "Audio.VoiceStealing";
    std::vector<VoiceCandidate> cands = {
        {1, "e", 10, 0.9f, 1, false}, {2, "e", 50, 0.1f, 2, false}, {3, "e", 50, 0.5f, 3, false}};
    auto pick = selectVoiceToSteal(cands, false);
    if (!pick || cands[*pick].handle != 1) return fail(name, "lowest priority not stolen");
    cands.erase(cands.begin());
    pick = selectVoiceToSteal(cands, false);
    if (!pick || cands[*pick].handle != 2) return fail(name, "quietest not stolen on priority tie");
    std::vector<VoiceCandidate> loops = {{9, "e", 1, 0.1f, 1, true}};
    if (selectVoiceToSteal(loops, false).has_value()) return fail(name, "loop stolen by one-shot");
    // Global cap integration: 3rd (high priority) steals lowest-priority voice.
    AudioEventSystem sys; MockAudioBackend mock;
    sys.setPlayFn(mock.playFn()); sys.setStopFn(mock.stopFn()); sys.setSeed(2);
    sys.setBudget({2, 8});
    sys.registerEvent(makeTestDef("low", 10)); sys.registerEvent(makeTestDef("mid", 50)); sys.registerEvent(makeTestDef("high", 90));
    sys.play2D("low"); sys.play2D("mid");
    if (sys.activeVoiceCount() != 2) return fail(name, "setup voices wrong");
    sys.play2D("high");
    if (sys.activeVoiceCount() != 2) return fail(name, "cap exceeded");
    if (sys.activeVoiceCountForEvent("low") != 0) return fail(name, "lowest-priority voice was not stolen");
    if (mock.stopped.size() != 1) return fail(name, "stolen voice not stopped");
    return pass(name);
}

bool testAudioPerEventCap()
{
    const std::string name = "Audio.PerEventCap";
    AudioEventSystem sys; MockAudioBackend mock;
    sys.setPlayFn(mock.playFn()); sys.setStopFn(mock.stopFn()); sys.setSeed(3);
    sys.registerEvent(makeTestDef("capped", 50, 2));
    sys.play2D("capped"); sys.play2D("capped"); sys.play2D("capped"); sys.play2D("capped");
    if (sys.activeVoiceCountForEvent("capped") != 2) return fail(name, "per-event cap not held");
    if (mock.stopped.empty()) return fail(name, "cap did not steal within event");
    return pass(name);
}

bool testAudioClipPickNoRepeat()
{
    const std::string name = "Audio.ClipPickNoRepeat";
    for (uint32_t r = 0; r < 1000; ++r)
    {
        int idx = pickClipIndexNoRepeat(4, 2, r);
        if (idx == 2 || idx < 0 || idx >= 4) return fail(name, "pure pick repeated/out of range");
    }
    AudioEventSystem sys; MockAudioBackend mock;
    sys.setPlayFn(mock.playFn()); sys.setStopFn(mock.stopFn()); sys.setSeed(42);
    sys.setBudget({1000, 1000});
    auto def = makeTestDef("multi"); def.max_voices = 1000; sys.registerEvent(def);
    for (int i = 0; i < 50; ++i) sys.play2D("multi");
    if (mock.clips.size() != 50) return fail(name, "not all plays captured");
    for (size_t i = 1; i < mock.clips.size(); ++i)
        if (mock.clips[i] == mock.clips[i - 1]) return fail(name, "consecutive clip repeat");
    return pass(name);
}

bool testAudioDistanceCullingAndLookup()
{
    const std::string name = "Audio.DistanceCulling";
    AudioEventSystem sys; MockAudioBackend mock;
    sys.setPlayFn(mock.playFn()); sys.setStopFn(mock.stopFn()); sys.setSeed(4);
    sys.registerEvent(makeTestDef("evt"));
    if (!sys.findEvent("evt") || sys.findEvent("evt")->max_distance != 40.0f) return fail(name, "table lookup wrong");
    sys.setListenerPosition(glm::vec3(0));
    if (sys.playAt("evt", glm::vec3(80, 0, 0)) != INVALID_SOUND) return fail(name, "2x distance not culled");
    if (mock.play_calls != 0 || sys.activeVoiceCount() != 0) return fail(name, "culled event started a voice");
    if (sys.playAt("evt", glm::vec3(20, 0, 0)) == INVALID_SOUND) return fail(name, "0.5x distance did not play");
    if (sys.activeVoiceCount() != 1) return fail(name, "in-range voice not tracked");
    float nan = std::numeric_limits<float>::quiet_NaN();
    if (sys.playAt("evt", glm::vec3(nan, 0, 0)) != INVALID_SOUND) return fail(name, "NaN position not rejected");
    return pass(name);
}

bool testAudioServerExclusionHeadless()
{
    const std::string name = "Audio.ServerExclusion";
    // This test process never initializes audio (dedicated-server analogue).
    if (AudioSystem::get().isInitialized()) return fail(name, "audio initialized in headless test");
    if (AudioSystem::get().playSound(INVALID_AUDIO_CLIP) != INVALID_SOUND) return fail(name, "headless play not INVALID");
    float nan = std::numeric_limits<float>::quiet_NaN();
    if (AudioSystem::get().setSoundPosition(1, glm::vec3(nan, 0, 0))) return fail(name, "NaN position accepted");
    AudioEventSystem sys; sys.registerEvent(makeTestDef("evt")); // default backend
    if (sys.play2D("evt") != INVALID_SOUND || sys.activeVoiceCount() != 0) return fail(name, "default backend started headless voice");
    // Source system is a safe no-op headless, including after entity death.
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<TransformComponent>(e, 1.0f, 2.0f, 3.0f);
    auto& src = reg.emplace<AudioSourceComponent>(e);
    src.clip_path = "missing.wav"; src.auto_play = true;
    AudioSourceSystem::update(reg, 0.016f);
    reg.destroy(e);
    AudioSourceSystem::update(reg, 0.016f);
    return pass(name);
}

bool testAudioSourceFollowUsesSimulationHierarchyPose()
{
    const std::string name = "Audio.SourceFollow";
    auto& audio = AudioSystem::get();
    const bool initialized_here = !audio.isInitialized();
    if (initialized_here && !audio.initialize(AudioBackendMode::NoDevice))
        return fail(name, "audio backend did not initialize");

    entt::registry reg;
    const auto wav_path = std::filesystem::temp_directory_path() /
        ("garden_audio_source_follow_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    AudioClipId clip = INVALID_AUDIO_CLIP;
    const auto cleanup = [&]() {
        AudioSourceSystem::shutdown(reg);
        if (clip != INVALID_AUDIO_CLIP)
            audio.unloadClip(clip);
        if (initialized_here)
            audio.shutdown();
        std::error_code error;
        std::filesystem::remove(wav_path, error);
    };
    const auto fail_with_cleanup = [&](const char* reason) {
        cleanup();
        return fail(name, reason);
    };

    if (!writeSilentTestWav(wav_path))
        return fail_with_cleanup("could not create test clip");
    clip = audio.loadClip(wav_path.string());
    if (clip == INVALID_AUDIO_CLIP)
        return fail_with_cleanup("could not load test clip");

    const entt::entity parent = reg.create();
    const entt::entity child = reg.create();
    reg.emplace<IdentityComponent>(parent,
        IdentityComponent{EntityGuid{0x4000000000000001ull, 0x5000000000000001ull}});
    reg.emplace<IdentityComponent>(child,
        IdentityComponent{EntityGuid{0x4000000000000002ull, 0x5000000000000002ull}});
    reg.emplace<TransformComponent>(parent, 10.0f, 2.0f, -3.0f);
    reg.emplace<TransformComponent>(child, 1.0f, 4.0f, 2.0f);
    auto& source = reg.emplace<AudioSourceComponent>(child);
    source.clip = clip;
    source.spatial = true;
    source.loop = true;
    source.auto_play = true;
    if (!Hierarchy::attach(reg, child, parent))
        return fail_with_cleanup("failed to create source hierarchy");

    // Render interpolation must not affect the simulation position heard by audio.
    reg.emplace<PresentationTransformComponent>(parent,
        PresentationTransformComponent{{100.0f, 200.0f, 300.0f}, {0.0f, 45.0f, 0.0f}});
    reg.emplace<PresentationTransformComponent>(child,
        PresentationTransformComponent{{400.0f, 500.0f, 600.0f}, {0.0f, 0.0f, 45.0f}});

    const size_t voices_before = audio.activeVoiceCount();
    AudioSourceSystem::update(reg, 0.016f);
    if (!source.started || source.active_handle == INVALID_SOUND ||
        audio.activeVoiceCount() != voices_before + 1 ||
        !approxVec3(audio.getSoundPosition(source.active_handle), glm::vec3(11.0f, 6.0f, -1.0f)))
    {
        return fail_with_cleanup("spatial source did not start at the composed simulation position");
    }

    reg.get<TransformComponent>(parent).position = {20.0f, -1.0f, 7.0f};
    reg.get<PresentationTransformComponent>(parent).position = {-100.0f, -200.0f, -300.0f};
    reg.get<PresentationTransformComponent>(child).position = {-400.0f, -500.0f, -600.0f};
    AudioSourceSystem::update(reg, 0.016f);
    if (!approxVec3(audio.getSoundPosition(source.active_handle), glm::vec3(21.0f, 3.0f, 9.0f)))
        return fail_with_cleanup("parent motion or presentation pose changed the source position incorrectly");

    const SoundHandle handle = source.active_handle;
    reg.destroy(child);
    AudioSourceSystem::update(reg, 0.016f);
    if (audio.activeVoiceCount() != voices_before || audio.isSoundPlaying(handle))
        return fail_with_cleanup("destroyed source was not stopped within one update");

    cleanup();
    return pass(name);
}
