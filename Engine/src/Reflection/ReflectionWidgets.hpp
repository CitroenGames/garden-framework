#pragma once

#include "EngineGraphicsExport.h"
#include "ReflectionTypes.hpp"

// Draw the appropriate ImGui widget for a reflected property.
// `component` is a raw pointer to the start of the component instance.
// Returns true if the value was modified.
// If out_edit_started is non-null, sets it to true when a drag/edit begins (for undo).
ENGINE_GRAPHICS_API bool drawReflectedProperty(const PropertyDescriptor& prop, void* component,
                                               bool* out_edit_started = nullptr);

// Draw all properties of a component using reflected metadata.
// Returns true if any value was modified.
// If out_edit_started is non-null, sets it to true when a drag/edit begins (for undo).
ENGINE_GRAPHICS_API bool drawReflectedComponent(const ComponentDescriptor& desc, void* component,
                                                bool* out_edit_started = nullptr);
