#pragma once

#include "Components/LocalPlayerInputComponent.hpp"
#include "InputActionSystem.hpp"
#include "Input/GameInputModeService.hpp"

namespace UiInput
{
    inline constexpr const char* ModalContextName = "FPSShooterModalUI";

    inline bool registerModalContext(InputActionSystem& actions)
    {
        MappingContext context;
        context.name = ModalContextName;
        context.priority = 100;
        context.modal = true;
        return actions.registerContext(context);
    }

    inline bool synchronizeModalContext(InputActionSystem& actions,
                                        bool modal,
                                        bool& active)
    {
        if (modal == active)
            return true;

        if (modal)
        {
            if (!actions.pushContext(ModalContextName))
                return false;
            active = true;
            return true;
        }

        actions.popContext(ModalContextName);
        active = false;
        return true;
    }

    // The service is optional because the headless server and editor hosts do
    // not expose a standalone game pointer.  Action isolation remains valid
    // there; a normal client host also changes its relative-pointer mode.
    inline bool synchronizeModalInputMode(IGameInputModeService* service,
                                          bool modal,
                                          bool& active)
    {
        if (modal == active)
            return true;

        if (!service)
        {
            active = modal;
            return true;
        }

        if (!service->requestGameInputMode(
                modal ? GameInputMode::UiPointer : GameInputMode::GameplayPointerCapture))
            return false;

        active = modal;
        return true;
    }

    inline bool synchronizeModalInput(InputActionSystem& actions,
                                      IGameInputModeService* service,
                                      bool modal,
                                      bool& context_active,
                                      bool& input_mode_active)
    {
        return synchronizeModalContext(actions, modal, context_active) &&
               synchronizeModalInputMode(service, modal, input_mode_active);
    }

    inline void suppressGameplayIntent(LocalPlayerInputComponent* local_input)
    {
        if (local_input)
            local_input->clearIntent();
    }
}
