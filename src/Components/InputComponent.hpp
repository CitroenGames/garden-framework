#pragma once

#include "InputManager.hpp"
#include <memory>

// Base class for any object that wants to receive input
class InputComponent
{
protected:
    std::shared_ptr<InputManager> input_manager;
    bool input_enabled = true;

public:
    InputComponent(std::shared_ptr<InputManager> input_mgr) : input_manager(input_mgr) {}
    virtual ~InputComponent() = default;

    // Override these in derived classes to set up input bindings
    virtual void setup_input_bindings() = 0;
    virtual void cleanup_input_bindings() {}

    // Enable/disable input for this component
    void set_input_enabled(bool enabled) { input_enabled = enabled; }
    bool is_input_enabled() const { return input_enabled; }

    // Get the input manager
    std::shared_ptr<InputManager> get_input_manager() const { return input_manager; }
};