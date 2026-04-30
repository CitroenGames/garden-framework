#include "NetworkRuntime.hpp"
#include "Utils/Log.hpp"
#include "enet.h"

#include <cstdint>
#include <mutex>

namespace Net {
namespace {
    std::mutex g_network_mutex;
    uint32_t g_ref_count = 0;
}

bool NetworkRuntime::acquire()
{
    std::lock_guard<std::mutex> lock(g_network_mutex);
    if (g_ref_count == 0) {
        if (enet_initialize() != 0) {
            LOG_ENGINE_ERROR("Failed to initialize ENet runtime");
            return false;
        }
    }
    ++g_ref_count;
    return true;
}

void NetworkRuntime::release()
{
    std::lock_guard<std::mutex> lock(g_network_mutex);
    if (g_ref_count == 0) {
        return;
    }
    --g_ref_count;
    if (g_ref_count == 0) {
        enet_deinitialize();
    }
}

bool NetworkRuntime::isInitialized()
{
    std::lock_guard<std::mutex> lock(g_network_mutex);
    return g_ref_count > 0;
}

} // namespace Net
