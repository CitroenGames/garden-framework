#pragma once
#include "Audio/AudioSystem.hpp"
#include "Audio/SoundEvent.hpp"
#include "Audio/SoundEventTable.hpp"
#include "Audio/AudioSourceSystem.hpp"
#include "Components/AudioSourceComponent.hpp"
#include "Components/IdentityComponent.hpp"
#include "ECS/Hierarchy.hpp"
#include "shared/FpsAudioEvents.hpp"
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

bool testAudioEventTableRoundTrip()
{
    const std::string name = "Audio.EventTableRoundTrip";
    SoundEventDef first = makeTestDef("ambient.wind", 42, 3);
    first.volume = 0.75f;
    first.pitch_min = 0.8f;
    first.pitch_max = 1.2f;
    first.group = AudioGroup::Music;
    first.min_distance = 2.0f;
    first.max_distance = 80.0f;
    SoundEventDef second = makeTestDef("ui.confirm", 80, 1);
    second.clip_paths = {"ui/confirm.wav"};
    second.group = AudioGroup::UI;

    SoundEventTable original;
    std::string error;
    if (!SoundEventTable::create(SoundEventTable::CurrentVersion,
                                 {first, second}, original, &error))
        return fail(name, "could not create source table: " + error);

    std::string serialized;
    if (!original.serialize(serialized, &error))
        return fail(name, "could not serialize table: " + error);

    SoundEventTable round_trip;
    if (!SoundEventTable::deserialize(serialized, round_trip, &error))
        return fail(name, "could not deserialize table: " + error);
    if (round_trip.version() != SoundEventTable::CurrentVersion ||
        round_trip.definitions().size() != 2)
        return fail(name, "round-trip table shape changed");
    const SoundEventDef& result = round_trip.definitions().front();
    if (result.name != first.name || result.clip_paths != first.clip_paths ||
        result.volume != first.volume || result.pitch_min != first.pitch_min ||
        result.pitch_max != first.pitch_max || result.group != first.group ||
        result.min_distance != first.min_distance || result.max_distance != first.max_distance ||
        result.priority != first.priority || result.max_voices != first.max_voices ||
        result.loop != first.loop)
        return fail(name, "round-trip definition changed");
    return pass(name);
}

bool testAudioEventTableRejectsMalformedDefinitions()
{
    const std::string name = "Audio.EventTableMalformed";
    const std::vector<std::string> documents = {
        "{",
        R"({"schema":"garden.sound-events","version":2,"events":[]})",
        R"({"schema":"other","version":1,"events":[]})",
        R"({"schema":"garden.sound-events","version":1,"events":[{"id":"bad","clips":[],"volume":1,"pitch_min":1,"pitch_max":1,"group":"sfx","min_distance":1,"max_distance":2,"priority":50,"max_voices":1,"loop":false}]})",
        R"({"schema":"garden.sound-events","version":1,"events":[{"id":"bad","clips":["clip.wav"],"volume":-1,"pitch_min":1,"pitch_max":1,"group":"sfx","min_distance":1,"max_distance":2,"priority":50,"max_voices":1,"loop":false}]})",
        R"({"schema":"garden.sound-events","version":1,"events":[{"id":"bad","clips":["clip.wav"],"volume":1,"pitch_min":1.1,"pitch_max":1,"group":"sfx","min_distance":1,"max_distance":2,"priority":50,"max_voices":1,"loop":false}]})",
        R"({"schema":"garden.sound-events","version":1,"events":[{"id":"bad","clips":["clip.wav"],"volume":1,"pitch_min":1,"pitch_max":1,"group":"sfx","min_distance":4,"max_distance":2,"priority":50,"max_voices":1,"loop":false}]})",
        R"({"schema":"garden.sound-events","version":1,"events":[{"id":"bad","clips":["clip.wav"],"volume":1,"pitch_min":1,"pitch_max":1,"group":"sfx","min_distance":1,"max_distance":2,"priority":101,"max_voices":1,"loop":false}]})",
        R"({"schema":"garden.sound-events","version":1,"events":[{"id":"bad","clips":["clip.wav"],"volume":1,"pitch_min":1,"pitch_max":1,"group":"sfx","min_distance":1,"max_distance":2,"priority":50,"max_voices":-1,"loop":false}]})",
        R"({"schema":"garden.sound-events","version":1,"events":[{"id":"bad","clips":["clip.wav"],"volume":1,"pitch_min":1,"pitch_max":1,"group":"invalid","min_distance":1,"max_distance":2,"priority":50,"max_voices":1,"loop":false}]})",
    };
    for (const std::string& document : documents)
    {
        SoundEventTable rejected;
        std::string error;
        if (SoundEventTable::deserialize(document, rejected, &error) || error.empty())
            return fail(name, "malformed document was accepted without a named error");
    }

    SoundEventDef duplicate = makeTestDef("duplicate");
    SoundEventTable rejected;
    std::string error;
    if (SoundEventTable::create(SoundEventTable::CurrentVersion, {duplicate, duplicate}, rejected, &error) ||
        error.find("duplicate") == std::string::npos)
        return fail(name, "duplicate event id was not rejected deterministically");
    return pass(name);
}

bool testAudioEventTableReplacementKeepsActiveVoices()
{
    const std::string name = "Audio.EventTableReplacement";
    AudioEventSystem sys;
    MockAudioBackend mock;
    sys.setPlayFn(mock.playFn());
    sys.setStopFn(mock.stopFn());
    sys.setSeed(9);

    SoundEventTable first;
    SoundEventTable second;
    std::string error;
    if (!SoundEventTable::create(1, {makeTestDef("before")}, first, &error) ||
        !SoundEventTable::create(1, {makeTestDef("after")}, second, &error) ||
        !sys.replaceEventTable(first, &error))
        return fail(name, "could not install initial table: " + error);
    if (sys.play2D("before") == INVALID_SOUND || sys.activeVoiceCount() != 1)
        return fail(name, "initial table did not start a voice");
    if (!sys.replaceEventTable(second, &error))
        return fail(name, "replacement table rejected: " + error);
    if (sys.activeVoiceCount() != 1 || !mock.stopped.empty())
        return fail(name, "replacement invalidated an active voice");
    if (sys.play2D("before") != INVALID_SOUND || sys.play2D("after") == INVALID_SOUND)
        return fail(name, "replacement did not atomically change future lookups");
    return pass(name);
}

bool testFpsAudioEventTableRegistration()
{
    const std::string name = "Audio.FpsTemplateEventTable";
    const SoundEventTable& table = fpsAudioEventTable();
    std::string error;
    if (!table.validate(&error) || table.version() != SoundEventTable::CurrentVersion)
        return fail(name, "template table is invalid: " + error);

    AudioEventSystem sys;
    if (!sys.replaceEventTable(table, &error) || sys.eventCount() != 5)
        return fail(name, "template table was not registered: " + error);
    constexpr const char* referenced_events[] = {
        "weapon.fire", "player.footstep", "player.hurt", "player.death", "ui.click",
    };
    for (const char* event_name : referenced_events)
    {
        const SoundEventDef* definition = sys.findEvent(event_name);
        if (!definition || definition->clip_paths.empty() || definition->clip_paths.front().empty())
            return fail(name, "template event has no clip binding: " + std::string(event_name));
    }
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

bool testAudioSourceLifecycleHardening()
{
    const std::string name = "Audio.SourceLifecycle";
    auto& audio = AudioSystem::get();
    const bool initialized_here = !audio.isInitialized();
    entt::registry reg;
    const auto wav_path = std::filesystem::temp_directory_path() /
        ("garden_audio_source_lifecycle_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    const auto cleanup = [&]() {
        AudioSourceSystem::shutdown(reg);
        if (initialized_here && audio.isInitialized()) audio.shutdown();
        std::error_code error;
        std::filesystem::remove(wav_path, error);
    };
    const auto fail_with_cleanup = [&](const char* reason) {
        cleanup();
        return fail(name, reason);
    };

    if (!writeSilentTestWav(wav_path))
        return fail_with_cleanup("could not create test clip");

    const entt::entity source_entity = reg.create();
    reg.emplace<TransformComponent>(source_entity, 1.0f, 2.0f, 3.0f);
    auto& source = reg.emplace<AudioSourceComponent>(source_entity);
    source.clip_path = wav_path.string();
    source.auto_play = true;
    source.loop = true;

    // A loaded level may contain sources before an audio device is available.
    // The source must remain pending rather than recording a failed start.
    AudioSourceSystem::update(reg, 0.016f);
    if (source.started || source.active_handle != INVALID_SOUND)
        return fail_with_cleanup("source started before audio initialization");
    if (initialized_here && !audio.initialize(AudioBackendMode::NoDevice))
        return fail_with_cleanup("audio backend did not initialize");
    AudioSourceSystem::update(reg, 0.016f);
    if (!source.started || source.active_handle == INVALID_SOUND || audio.activeVoiceCount() != 1)
        return fail_with_cleanup("pending source did not start after audio initialization");

    const SoundHandle first_handle = source.active_handle;
    source.enabled = false;
    AudioSourceSystem::update(reg, 0.016f);
    if (source.started || source.active_handle != INVALID_SOUND || audio.activeVoiceCount() != 0)
        return fail_with_cleanup("disabling a source did not stop its loop");

    source.enabled = true;
    AudioSourceSystem::update(reg, 0.016f);
    if (!source.started || source.active_handle == INVALID_SOUND || source.active_handle == first_handle)
        return fail_with_cleanup("re-enabling a source did not restart its loop");

    source.volume = 0.25f;
    source.pitch = 1.5f;
    reg.get<TransformComponent>(source_entity).position = {7.0f, 8.0f, 9.0f};
    AudioSourceSystem::update(reg, 0.016f);
    if (audio.getSoundVolume(source.active_handle) != source.volume ||
        audio.getSoundPitch(source.active_handle) != source.pitch ||
        !approxVec3(audio.getSoundPosition(source.active_handle), glm::vec3(7.0f, 8.0f, 9.0f)))
        return fail_with_cleanup("live source gain, pitch, or position was not applied");

    const SoundHandle loop_handle = source.active_handle;
    source.loop = false;
    AudioSourceSystem::update(reg, 0.016f);
    if (!source.started || source.active_handle == INVALID_SOUND || source.active_handle == loop_handle)
        return fail_with_cleanup("changing loop mode did not restart the source");

    source.auto_play = false;
    AudioSourceSystem::update(reg, 0.016f);
    if (source.started || audio.activeVoiceCount() != 0)
        return fail_with_cleanup("disabling auto play did not stop the source");

    // Component replacement on the same entity must not leave the old voice
    // tracked under that entity identifier.
    reg.remove<AudioSourceComponent>(source_entity);
    AudioSourceSystem::update(reg, 0.016f);
    auto& readded = reg.emplace<AudioSourceComponent>(source_entity);
    readded.clip_path = wav_path.string();
    readded.auto_play = true;
    readded.loop = true;
    AudioSourceSystem::update(reg, 0.016f);
    if (!readded.started || readded.active_handle == INVALID_SOUND || audio.activeVoiceCount() != 1)
        return fail_with_cleanup("re-added component did not own exactly one source voice");

    const SoundHandle readded_handle = readded.active_handle;
    reg.destroy(source_entity);
    AudioSourceSystem::update(reg, 0.016f);
    if (audio.activeVoiceCount() != 0 || audio.isSoundPlaying(readded_handle))
        return fail_with_cleanup("destroyed source did not stop its loop");

    // An unresolved direct clip id is a pending source, not a fake playback.
    const entt::entity pending_entity = reg.create();
    reg.emplace<TransformComponent>(pending_entity);
    auto& pending = reg.emplace<AudioSourceComponent>(pending_entity);
    pending.clip = 0x7fffffffu;
    pending.auto_play = true;
    pending.loop = true;
    AudioSourceSystem::update(reg, 0.016f);
    if (pending.started || pending.active_handle != INVALID_SOUND || audio.activeVoiceCount() != 0)
        return fail_with_cleanup("unready clip started a source voice");

    // Project reload first clears ECS runtime state, then a restarted mixer
    // must restore the authored source from its path instead of reusing stale
    // handles or stale clip ids.
    reg.destroy(pending_entity);
    const entt::entity reload_entity = reg.create();
    reg.emplace<TransformComponent>(reload_entity, -2.0f, 0.0f, 4.0f);
    auto& reload_source = reg.emplace<AudioSourceComponent>(reload_entity);
    reload_source.clip_path = wav_path.string();
    reload_source.auto_play = true;
    reload_source.loop = true;
    AudioSourceSystem::update(reg, 0.016f);
    if (!reload_source.started || audio.activeVoiceCount() != 1)
        return fail_with_cleanup("reload source did not start before project reload");
    AudioSourceSystem::shutdown(reg);
    if (reload_source.started || reload_source.active_handle != INVALID_SOUND || audio.activeVoiceCount() != 0)
        return fail_with_cleanup("project shutdown retained source runtime state");
    audio.shutdown();
    if (!audio.initialize(AudioBackendMode::NoDevice))
        return fail_with_cleanup("audio backend did not reinitialize after project reload");
    AudioSourceSystem::update(reg, 0.016f);
    if (!reload_source.started || reload_source.active_handle == INVALID_SOUND || audio.activeVoiceCount() != 1)
        return fail_with_cleanup("project reload did not restore authored source");

    cleanup();
    return pass(name);
}
