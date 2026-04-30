#pragma once

#include "EngineExport.h"

namespace Net {

class ENGINE_API NetworkRuntime
{
public:
    static bool acquire();
    static void release();
    static bool isInitialized();

private:
    NetworkRuntime() = delete;
};

} // namespace Net
