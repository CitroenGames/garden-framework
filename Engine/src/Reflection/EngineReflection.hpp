#pragma once

#include "EngineExport.h"

class ReflectionRegistry;

// Register all built-in engine components with the reflection registry.
// Call once during engine initialization.
ENGINE_API void registerEngineReflection(ReflectionRegistry& registry);
