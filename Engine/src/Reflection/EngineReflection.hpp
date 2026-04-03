#pragma once

class ReflectionRegistry;

// Register all built-in engine components with the reflection registry.
// Call once during engine initialization.
void registerEngineReflection(ReflectionRegistry& registry);
