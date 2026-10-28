#include "AssetManager.hpp"
#include "Utils/Log.hpp"
#include <filesystem>
#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

namespace Assets {

AssetManager& AssetManager::get() {
    static AssetManager instance;
    return instance;
}

AssetManager::~AssetManager() {
    shutdown();
}

bool AssetManager::initialize(IRenderAPI* render_api) {
    if (m_initialized.exchange(true)) {
        LOG_ENGINE_WARN("AssetManager: Already initialized");
        return true;
    }

    m_render_api = render_api;

    LOG_ENGINE_INFO("AssetManager: Initialized");
    return true;
}

void AssetManager::shutdown() {
    if (!m_initialized.exchange(false)) {
        return;
    }

    LOG_ENGINE_INFO("AssetManager: Shutting down...");

    {
        std::unique_lock<std::shared_mutex> lock(m_assets_mutex);
        m_assets.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_path_mutex);
        m_path_to_id.clear();
    }

    m_loaders.clear();
    m_render_api = nullptr;

    LOG_ENGINE_INFO("AssetManager: Shutdown complete");
}

namespace {

// Asset paths are resource identities. Normalize spellings before they reach
// the cache, but retain logical case rather than folding it.
std::string normalizeSeparators(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

bool isWithinRoot(const fs::path& root, const fs::path& candidate)
{
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty())
        return candidate == root;
    if (relative.is_absolute())
        return false;

    for (const auto& component : relative) {
        if (component == "..")
            return false;
    }
    return true;
}

fs::path canonicalOrLexical(const fs::path& path)
{
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

} // namespace

void AssetManager::setAssetRoot(const std::string& root) {
    if (root.empty()) {
        m_asset_root.clear();
        return;
    }

    std::error_code ec;
    fs::path absolute = fs::absolute(fs::path(normalizeSeparators(root)), ec);
    m_asset_root = canonicalOrLexical(ec ? fs::path(normalizeSeparators(root)) : absolute).generic_string();
    LOG_ENGINE_INFO("AssetManager: Asset root set to '{}'", m_asset_root);
}

void AssetManager::setAssetPrefix(const std::string& prefix) {
    m_asset_prefix = normalizeSeparators(prefix);
    if (!m_asset_prefix.empty() && m_asset_prefix.back() != '/')
        m_asset_prefix += '/';
}

std::string AssetManager::resolveAssetPath(const std::string& path) const {
    if (path.empty())
        return {};

    std::string resolved = normalizeSeparators(path);
    if (!m_asset_prefix.empty() && resolved.size() >= m_asset_prefix.size() &&
        resolved.compare(0, m_asset_prefix.size(), m_asset_prefix) == 0) {
        resolved = resolved.substr(m_asset_prefix.size());
    }

    fs::path candidate(resolved);
    if (m_asset_root.empty())
        return candidate.lexically_normal().generic_string();

    const fs::path root(m_asset_root);
    if (!candidate.is_absolute())
        candidate = root / candidate;
    candidate = canonicalOrLexical(candidate);

    // Resolve symlinks before containment, so a link in the project cannot
    // make an outside resource appear project-owned.
    if (!isWithinRoot(root, candidate)) {
        LOG_ENGINE_ERROR("AssetManager: Asset path '{}' escapes configured root '{}'", path, m_asset_root);
        return {};
    }

    return candidate.generic_string();
}

void AssetManager::registerLoader(std::unique_ptr<IAssetLoader> loader) {
    if (!loader) return;

    LOG_ENGINE_INFO("AssetManager: Registered loader for {} assets (source: {})",
                   assetTypeToString(loader->getAssetType()),
                   loader->getSourceId());

    m_loaders.push_back(std::move(loader));
}

void AssetManager::unregisterLoadersFromSource(const char* source_id) {
    if (!source_id) return;

    size_t before = m_loaders.size();
    m_loaders.erase(
        std::remove_if(m_loaders.begin(), m_loaders.end(),
            [source_id](const std::unique_ptr<IAssetLoader>& l) {
                return l && std::strcmp(l->getSourceId(), source_id) == 0;
            }),
        m_loaders.end());

    size_t removed = before - m_loaders.size();
    if (removed > 0)
        LOG_ENGINE_INFO("AssetManager: Removed {} loader(s) from source '{}'", removed, source_id);
}

IAssetLoader* AssetManager::findLoaderForPath(const std::string& path) const {
    for (const auto& loader : m_loaders) {
        if (loader->canLoad(path)) {
            return loader.get();
        }
    }
    return nullptr;
}

AssetHandle AssetManager::loadAsync(const std::string& path,
                                   LoadPriority priority,
                                   LoadCallback on_complete,
                                   ProgressCallback on_progress) {
    if (!m_initialized) {
        LOG_ENGINE_ERROR("AssetManager: Not initialized");
        return AssetHandle();
    }

    const std::string canonical_path = resolveAssetPath(path);
    if (canonical_path.empty()) {
        LOG_ENGINE_ERROR("AssetManager: Invalid asset path '{}'", path);
        return AssetHandle();
    }

    AssetId id = INVALID_ASSET_ID;
    {
        // Existing identities remain retrievable even after their loader is
        // unregistered. The loader only governs creation of a new resource.
        std::scoped_lock lock(m_assets_mutex, m_path_mutex);
        const auto existing = m_path_to_id.find(canonical_path);
        if (existing != m_path_to_id.end())
            id = existing->second;
    }

    if (id != INVALID_ASSET_ID) {
        AssetState* state = getAssetState(id);
        if (state && state->state.load(std::memory_order_acquire) == LoadState::Ready && on_complete)
            on_complete(id, true, state->data);
        return AssetHandle(id, this);
    }

    IAssetLoader* loader = findLoaderForPath(canonical_path);
    if (!loader) {
        LOG_ENGINE_ERROR("AssetManager: No loader found for '{}'", canonical_path);
        return AssetHandle();
    }

    bool created = false;
    {
        // Recheck after loader selection, then insert state and key together.
        // Every lifecycle state is represented by one identity even for racing callers.
        std::scoped_lock lock(m_assets_mutex, m_path_mutex);
        const auto existing = m_path_to_id.find(canonical_path);
        if (existing != m_path_to_id.end()) {
            id = existing->second;
        } else {
            id = m_next_id.fetch_add(1, std::memory_order_relaxed);
            auto asset_state = std::make_unique<AssetState>();
            asset_state->id = id;
            asset_state->path = canonical_path;
            asset_state->type = loader->getAssetType();
            asset_state->state.store(LoadState::Queued, std::memory_order_release);
            asset_state->on_complete = std::move(on_complete);
            asset_state->on_progress = std::move(on_progress);
            m_assets.emplace(id, std::move(asset_state));
            m_path_to_id.emplace(canonical_path, id);
            created = true;
        }
    }

    if (!created) {
        AssetState* state = getAssetState(id);
        if (state && state->state.load(std::memory_order_acquire) == LoadState::Ready && on_complete)
            on_complete(id, true, state->data);
        return AssetHandle(id, this);
    }

    const std::string base_path = canonical_path.substr(0, canonical_path.find_last_of("/\\") + 1);

    Threading::JobPriority job_priority = Threading::JobPriority::Normal;
    if (priority == LoadPriority::High || priority == LoadPriority::Immediate) {
        job_priority = Threading::JobPriority::High;
    } else if (priority == LoadPriority::Background) {
        job_priority = Threading::JobPriority::Low;
    }

    auto load_job = Threading::JobSystem::get().createJob()
        .setName("Load: " + canonical_path)
        .setPriority(job_priority)
        .setContext(Threading::JobContext::Worker)
        .setWork([this, id, canonical_path, base_path, loader]() {
            updateProgress(id, 0.1f, LoadState::LoadingIO);

            LoadContext context;
            context.render_api = m_render_api;
            context.base_path = base_path;
            context.verbose_logging = false;

            updateProgress(id, 0.3f, LoadState::Parsing);

            LoadResult result = loader->loadFromFile(canonical_path, context);

            if (!result.success) {
                failLoad(id, result.error_message);
                return;
            }

            updateProgress(id, 0.7f, LoadState::Processing);

            {
                std::unique_lock<std::shared_mutex> lock(m_assets_mutex);
                auto it = m_assets.find(id);
                if (it != m_assets.end()) {
                    it->second->data = result.data;
                }
            }

            updateProgress(id, 0.8f, LoadState::UploadingGPU);

            auto gpu_job = Threading::JobSystem::get().createJob()
                .setName("GPU Upload: " + canonical_path)
                .setPriority(Threading::JobPriority::High)
                .setContext(Threading::JobContext::MainThread)
                .setWork([this, id, loader]() {
                    AssetState* state = getAssetState(id);
                    if (!state) return;

                    bool success = loader->uploadToGPU(state->data, m_render_api);

                    if (success) {
                        completeLoad(id, true, state->data);
                    } else {
                        failLoad(id, "GPU upload failed");
                    }
                })
                .submit();

            {
                std::unique_lock<std::shared_mutex> lock(m_assets_mutex);
                auto it = m_assets.find(id);
                if (it != m_assets.end()) {
                    it->second->gpu_job = gpu_job;
                }
            }
        })
        .submit();

    {
        std::unique_lock<std::shared_mutex> lock(m_assets_mutex);
        auto it = m_assets.find(id);
        if (it != m_assets.end()) {
            it->second->load_job = load_job;
        }
    }

    return AssetHandle(id, this);
}

AssetHandle AssetManager::loadSync(const std::string& path) {
    AssetHandle handle = loadAsync(path, LoadPriority::Immediate);
    if (handle.isValid()) {
        handle.wait();
    }
    return handle;
}

std::vector<AssetHandle> AssetManager::loadBatch(const std::vector<std::string>& paths,
                                                LoadPriority priority) {
    std::vector<AssetHandle> handles;
    handles.reserve(paths.size());

    for (const auto& path : paths) {
        handles.push_back(loadAsync(path, priority));
    }

    return handles;
}

void AssetManager::updateProgress(AssetId id, float progress, LoadState state) {
    std::shared_lock<std::shared_mutex> lock(m_assets_mutex);
    auto it = m_assets.find(id);
    if (it != m_assets.end()) {
        it->second->progress.store(progress, std::memory_order_release);
        it->second->state.store(state, std::memory_order_release);

        if (it->second->on_progress) {
            it->second->on_progress(id, progress, state);
        }
    }
}

void AssetManager::completeLoad(AssetId id, bool success, const AssetData& data) {
    std::unique_lock<std::shared_mutex> lock(m_assets_mutex);
    auto it = m_assets.find(id);
    if (it != m_assets.end()) {
        it->second->progress.store(1.0f, std::memory_order_release);
        it->second->state.store(LoadState::Ready, std::memory_order_release);

        try {
            it->second->promise.set_value(data);
        } catch (...) {
        }

        if (it->second->on_complete) {
            it->second->on_complete(id, success, data);
        }

        LOG_ENGINE_INFO("AssetManager: Loaded '{}'", it->second->path);
    }
}

void AssetManager::failLoad(AssetId id, const std::string& error) {
    std::unique_lock<std::shared_mutex> lock(m_assets_mutex);
    auto it = m_assets.find(id);
    if (it != m_assets.end()) {
        it->second->state.store(LoadState::Failed, std::memory_order_release);
        it->second->error = AssetError(error, it->second->path);

        try {
            it->second->promise.set_value(AssetData{});
        } catch (...) {
        }

        if (it->second->on_complete) {
            it->second->on_complete(id, false, AssetData{});
        }

        LOG_ENGINE_ERROR("AssetManager: Failed to load '{}': {}", it->second->path, error);
    }
}

AssetState* AssetManager::getAssetState(AssetId id) const {
    std::shared_lock<std::shared_mutex> lock(m_assets_mutex);
    auto it = m_assets.find(id);
    return (it != m_assets.end()) ? it->second.get() : nullptr;
}

LoadState AssetManager::getLoadState(AssetId id) const {
    AssetState* s = getAssetState(id);
    return s ? s->state.load(std::memory_order_acquire) : LoadState::Failed;
}

float AssetManager::getProgress(AssetId id) const {
    AssetState* s = getAssetState(id);
    return s ? s->progress.load(std::memory_order_acquire) : 0.0f;
}

AssetData AssetManager::getData(AssetId id) const {
    AssetState* state = getAssetState(id);
    return state ? state->data : AssetData{};
}

std::optional<AssetError> AssetManager::getError(AssetId id) const {
    AssetState* state = getAssetState(id);
    return state ? state->error : std::nullopt;
}

bool AssetManager::isLoaded(const std::string& path) const {
    const std::string canonical_path = resolveAssetPath(path);
    if (canonical_path.empty()) return false;
    std::lock_guard<std::mutex> lock(m_path_mutex);
    auto it = m_path_to_id.find(canonical_path);
    if (it == m_path_to_id.end()) return false;

    AssetState* state = getAssetState(it->second);
    return state && state->state.load(std::memory_order_acquire) == LoadState::Ready;
}

AssetHandle AssetManager::getLoadedAsset(const std::string& path) const {
    const std::string canonical_path = resolveAssetPath(path);
    if (canonical_path.empty()) return AssetHandle();
    std::lock_guard<std::mutex> lock(m_path_mutex);
    auto it = m_path_to_id.find(canonical_path);
    if (it != m_path_to_id.end()) {
        return AssetHandle(it->second, const_cast<AssetManager*>(this));
    }
    return AssetHandle();
}

void AssetManager::unload(AssetId id) {
    std::string path;

    {
        std::unique_lock<std::shared_mutex> lock(m_assets_mutex);
        auto it = m_assets.find(id);
        if (it != m_assets.end()) {
            path = it->second->path;
            m_assets.erase(it);
        }
    }

    if (!path.empty()) {
        std::lock_guard<std::mutex> lock(m_path_mutex);
        m_path_to_id.erase(path);
    }
}

void AssetManager::unloadUnused() {
}

void AssetManager::clearCache() {
    {
        std::unique_lock<std::shared_mutex> lock(m_assets_mutex);
        m_assets.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_path_mutex);
        m_path_to_id.clear();
    }
}

void AssetManager::update() {
}

size_t AssetManager::getLoadingCount() const {
    std::shared_lock<std::shared_mutex> lock(m_assets_mutex);
    size_t count = 0;
    for (const auto& [id, state] : m_assets) {
        if (state->state.load(std::memory_order_acquire) != LoadState::Ready &&
            state->state.load(std::memory_order_acquire) != LoadState::Failed) {
            ++count;
        }
    }
    return count;
}

size_t AssetManager::getCachedCount() const {
    std::shared_lock<std::shared_mutex> lock(m_assets_mutex);
    return m_assets.size();
}

// AssetHandle implementation
AssetHandle::AssetHandle(AssetId id, AssetManager* manager)
    : m_id(id)
    , m_manager(manager)
{
}

LoadState AssetHandle::getState() const {
    return m_manager ? m_manager->getLoadState(m_id) : LoadState::Failed;
}

float AssetHandle::getProgress() const {
    return m_manager ? m_manager->getProgress(m_id) : 0.0f;
}

bool AssetHandle::isLoading() const {
    LoadState state = getState();
    return state != LoadState::NotLoaded && state != LoadState::Ready && state != LoadState::Failed;
}

bool AssetHandle::isReady() const {
    return getState() == LoadState::Ready;
}

bool AssetHandle::hasFailed() const {
    return getState() == LoadState::Failed;
}

std::optional<AssetError> AssetHandle::getError() const {
    return m_manager ? m_manager->getError(m_id) : std::nullopt;
}

AssetData AssetHandle::getData() const {
    return m_manager ? m_manager->getData(m_id) : AssetData{};
}

void AssetHandle::wait() const {
    if (!m_manager) return;

    AssetState* state = m_manager->getAssetState(m_id);
    if (state) {
        state->future.wait();
    }
}

bool AssetHandle::waitFor(std::chrono::milliseconds timeout) const {
    if (!m_manager) return false;

    AssetState* state = m_manager->getAssetState(m_id);
    if (state) {
        return state->future.wait_for(timeout) == std::future_status::ready;
    }
    return false;
}

} // namespace Assets
