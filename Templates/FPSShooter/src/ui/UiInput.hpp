#pragma once

#include "Components/LocalPlayerInputComponent.hpp"
#include "InputActionSystem.hpp"

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

    inline void suppressGameplayIntent(LocalPlayerInputComponent* local_input)
    {
        if (local_input)
            local_input->clearIntent();
    }
}
