#pragma once

#include <string>
#include <vector>

namespace beiklive::desktop_input
{
    struct CapturableKeyboardInput
    {
        int id;
        const char* name;
    };

    int keyboardInputIdForName(const std::string& name);
    const char* keyboardInputNameForId(int id);
    const std::vector<CapturableKeyboardInput>& extendedCapturableKeyboardInputs();
    std::vector<std::vector<int>> parseMixedInputCombos(const std::string& value);
}
