#include "AnimationResourceBindingSystem.hpp"

#include "Animation/AnimationResourceService.hpp"
#include "Components/AnimPlaybackComponent.hpp"
#include "Components/AnimResourceBindingComponent.hpp"
#include "Components/SkeletonComponent.hpp"
#include "Utils/Log.hpp"

namespace
{
bool sameKey(const AnimationResourceKey& left, const AnimationResourceKey& right)
{
    return left == right;
}

void clearPlayback(AnimPlaybackComponent& playback)
{
    playback.current_clip.reset();
    playback.blend_from_clip.reset();
    playback.time = 0.0f;
    playback.blend_from_time = 0.0f;
    playback.blend_duration = 0.0f;
    playback.blend_elapsed = 0.0f;
    playback.playing = false;
    playback.initialized = false;
    playback.completion_reported = false;
}
} // namespace

namespace AnimationResourceBindingSystem
{
void update(entt::registry& registry)
{
    AnimationResourceService& service = AnimationResourceService::get();
    auto view = registry.view<SkeletonComponent>();
    for (const entt::entity entity : view)
    {
        const SkeletonComponent& authored = view.get<SkeletonComponent>(entity);
        auto& binding = registry.get_or_emplace<AnimResourceBindingComponent>(entity);
        const AnimationResourceKey authored_key = authored.resourceKey();
        if (!binding.ticket.valid() || !sameKey(binding.requested_key, authored_key))
        {
            binding.requested_key = authored_key;
            binding.ticket = service.request(authored_key);
            binding.state = service.getState(binding.ticket);
            binding.diagnostic_emitted = false;
        }

        if (const auto ready = service.tryGetReady(binding.ticket))
        {
            if (binding.active != ready)
            {
                binding.active = ready;
                clearPlayback(registry.get_or_emplace<AnimPlaybackComponent>(entity));
            }
            binding.state = AnimationResourceState::Ready;
            binding.diagnostic_emitted = false;
            continue;
        }

        binding.state = service.getState(binding.ticket);
        if (binding.state == AnimationResourceState::Failed && !binding.diagnostic_emitted)
        {
            const auto error = service.getError(binding.ticket);
            LOG_ENGINE_WARN("Animation resource bind failed for entity {} path '{}': {} {}",
                            static_cast<uint32_t>(entity), authored_key.path,
                            error ? error->code : "ANIM_UNKNOWN", error ? error->message : "");
            binding.diagnostic_emitted = true;
        }
    }
}
} // namespace AnimationResourceBindingSystem
