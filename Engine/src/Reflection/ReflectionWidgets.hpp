#pragma once

#include "ReflectionTypes.hpp"

// Draw the appropriate ImGui widget for a reflected property.
// `component` is a raw pointer to the start of the component instance.
// Returns true if the value was modified.
bool drawReflectedProperty(const PropertyDescriptor& prop, void* component);

// Draw all properties of a component using reflected metadata.
// Returns true if any value was modified.
bool drawReflectedComponent(const ComponentDescriptor& desc, void* component);
