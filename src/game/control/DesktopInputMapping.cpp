#include "game/control/DesktopInputMapping.hpp"

#include "core/enums.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace beiklive::desktop_input
{
    namespace
    {
#if defined(__APPLE__) && !defined(__SWITCH__)
        inline constexpr RetroNameMap kExtendedKeyboardInputNames[] = {
            {"F13", brls::BRLS_KBD_KEY_F13}, {"F14", brls::BRLS_KBD_KEY_F14},
            {"F15", brls::BRLS_KBD_KEY_F15}, {"F16", brls::BRLS_KBD_KEY_F16},
            {"F17", brls::BRLS_KBD_KEY_F17}, {"F18", brls::BRLS_KBD_KEY_F18},
            {"F19", brls::BRLS_KBD_KEY_F19}, {"F20", brls::BRLS_KBD_KEY_F20},
            {"F21", brls::BRLS_KBD_KEY_F21}, {"F22", brls::BRLS_KBD_KEY_F22},
            {"F23", brls::BRLS_KBD_KEY_F23}, {"F24", brls::BRLS_KBD_KEY_F24},
            {"F25", brls::BRLS_KBD_KEY_F25},
            {"APOSTROPHE", brls::BRLS_KBD_KEY_APOSTROPHE},
            {"COMMA", brls::BRLS_KBD_KEY_COMMA},
            {"MINUS", brls::BRLS_KBD_KEY_MINUS},
            {"PERIOD", brls::BRLS_KBD_KEY_PERIOD},
            {"SLASH", brls::BRLS_KBD_KEY_SLASH},
            {"SEMICOLON", brls::BRLS_KBD_KEY_SEMICOLON},
            {"EQUAL", brls::BRLS_KBD_KEY_EQUAL},
            {"LEFTBRACKET", brls::BRLS_KBD_KEY_LEFT_BRACKET},
            {"BACKSLASH", brls::BRLS_KBD_KEY_BACKSLASH},
            {"RIGHTBRACKET", brls::BRLS_KBD_KEY_RIGHT_BRACKET},
            {"GRAVE", brls::BRLS_KBD_KEY_GRAVE_ACCENT},
            {"KP_ENTER", brls::BRLS_KBD_KEY_KP_ENTER},
            {"KPENTER", brls::BRLS_KBD_KEY_KP_ENTER},
            {"ESCAPE", brls::BRLS_KBD_KEY_ESCAPE},
            {"DELETE", brls::BRLS_KBD_KEY_DELETE},
            {"INSERT", brls::BRLS_KBD_KEY_INSERT},
            {"PAGEUP", brls::BRLS_KBD_KEY_PAGE_UP},
            {"PAGEDOWN", brls::BRLS_KBD_KEY_PAGE_DOWN},
            {"PGUP", brls::BRLS_KBD_KEY_PAGE_UP},
            {"PGDOWN", brls::BRLS_KBD_KEY_PAGE_DOWN},
            {"HOME", brls::BRLS_KBD_KEY_HOME}, {"END", brls::BRLS_KBD_KEY_END},
            {"CAPSLOCK", brls::BRLS_KBD_KEY_CAPS_LOCK},
            {"SCROLLLOCK", brls::BRLS_KBD_KEY_SCROLL_LOCK},
            {"NUMLOCK", brls::BRLS_KBD_KEY_NUM_LOCK},
            {"PRINTSCREEN", brls::BRLS_KBD_KEY_PRINT_SCREEN},
            {"PAUSE", brls::BRLS_KBD_KEY_PAUSE},
            {"KP0", brls::BRLS_KBD_KEY_KP_0}, {"KP1", brls::BRLS_KBD_KEY_KP_1},
            {"KP2", brls::BRLS_KBD_KEY_KP_2}, {"KP3", brls::BRLS_KBD_KEY_KP_3},
            {"KP4", brls::BRLS_KBD_KEY_KP_4}, {"KP5", brls::BRLS_KBD_KEY_KP_5},
            {"KP6", brls::BRLS_KBD_KEY_KP_6}, {"KP7", brls::BRLS_KBD_KEY_KP_7},
            {"KP8", brls::BRLS_KBD_KEY_KP_8}, {"KP9", brls::BRLS_KBD_KEY_KP_9},
            {"KPDECIMAL", brls::BRLS_KBD_KEY_KP_DECIMAL},
            {"KPDIVIDE", brls::BRLS_KBD_KEY_KP_DIVIDE},
            {"KPMULTIPLY", brls::BRLS_KBD_KEY_KP_MULTIPLY},
            {"KPSUBTRACT", brls::BRLS_KBD_KEY_KP_SUBTRACT},
            {"KPADD", brls::BRLS_KBD_KEY_KP_ADD},
            {"KPEQUAL", brls::BRLS_KBD_KEY_KP_EQUAL},
            {"WORLD1", brls::BRLS_KBD_KEY_WORLD_1},
            {"WORLD2", brls::BRLS_KBD_KEY_WORLD_2},
            {"LSHIFT", brls::BRLS_KBD_KEY_LEFT_SHIFT},
            {"RSHIFT", brls::BRLS_KBD_KEY_RIGHT_SHIFT},
            {"LCTRL", brls::BRLS_KBD_KEY_LEFT_CONTROL},
            {"RCTRL", brls::BRLS_KBD_KEY_RIGHT_CONTROL},
            {"LALT", brls::BRLS_KBD_KEY_LEFT_ALT},
            {"RALT", brls::BRLS_KBD_KEY_RIGHT_ALT},
            {"SUPER", brls::BRLS_KBD_KEY_LEFT_SUPER},
            {"LSUPER", brls::BRLS_KBD_KEY_LEFT_SUPER},
            {"RSUPER", brls::BRLS_KBD_KEY_RIGHT_SUPER},
            {"META", brls::BRLS_KBD_KEY_LEFT_SUPER},
            {"MENU", brls::BRLS_KBD_KEY_MENU},
        };

        const std::vector<CapturableKeyboardInput> kExtendedCapturableInputs = {
            {brls::BRLS_KBD_KEY_F13, "F13"}, {brls::BRLS_KBD_KEY_F14, "F14"},
            {brls::BRLS_KBD_KEY_F15, "F15"}, {brls::BRLS_KBD_KEY_F16, "F16"},
            {brls::BRLS_KBD_KEY_F17, "F17"}, {brls::BRLS_KBD_KEY_F18, "F18"},
            {brls::BRLS_KBD_KEY_F19, "F19"}, {brls::BRLS_KBD_KEY_F20, "F20"},
            {brls::BRLS_KBD_KEY_F21, "F21"}, {brls::BRLS_KBD_KEY_F22, "F22"},
            {brls::BRLS_KBD_KEY_F23, "F23"}, {brls::BRLS_KBD_KEY_F24, "F24"},
            {brls::BRLS_KBD_KEY_F25, "F25"},
            {brls::BRLS_KBD_KEY_APOSTROPHE, "APOSTROPHE"},
            {brls::BRLS_KBD_KEY_COMMA, "COMMA"},
            {brls::BRLS_KBD_KEY_MINUS, "MINUS"},
            {brls::BRLS_KBD_KEY_PERIOD, "PERIOD"},
            {brls::BRLS_KBD_KEY_SLASH, "SLASH"},
            {brls::BRLS_KBD_KEY_SEMICOLON, "SEMICOLON"},
            {brls::BRLS_KBD_KEY_EQUAL, "EQUAL"},
            {brls::BRLS_KBD_KEY_LEFT_BRACKET, "LEFTBRACKET"},
            {brls::BRLS_KBD_KEY_BACKSLASH, "BACKSLASH"},
            {brls::BRLS_KBD_KEY_RIGHT_BRACKET, "RIGHTBRACKET"},
            {brls::BRLS_KBD_KEY_GRAVE_ACCENT, "GRAVE"},
            {brls::BRLS_KBD_KEY_WORLD_1, "WORLD1"},
            {brls::BRLS_KBD_KEY_WORLD_2, "WORLD2"},
            {brls::BRLS_KBD_KEY_KP_ENTER, "KP_ENTER"},
            {brls::BRLS_KBD_KEY_INSERT, "INSERT"},
            {brls::BRLS_KBD_KEY_PAGE_UP, "PAGEUP"},
            {brls::BRLS_KBD_KEY_PAGE_DOWN, "PAGEDOWN"},
            {brls::BRLS_KBD_KEY_HOME, "HOME"}, {brls::BRLS_KBD_KEY_END, "END"},
            {brls::BRLS_KBD_KEY_CAPS_LOCK, "CAPSLOCK"},
            {brls::BRLS_KBD_KEY_SCROLL_LOCK, "SCROLLLOCK"},
            {brls::BRLS_KBD_KEY_NUM_LOCK, "NUMLOCK"},
            {brls::BRLS_KBD_KEY_PRINT_SCREEN, "PRINTSCREEN"},
            {brls::BRLS_KBD_KEY_PAUSE, "PAUSE"},
            {brls::BRLS_KBD_KEY_KP_0, "KP0"}, {brls::BRLS_KBD_KEY_KP_1, "KP1"},
            {brls::BRLS_KBD_KEY_KP_2, "KP2"}, {brls::BRLS_KBD_KEY_KP_3, "KP3"},
            {brls::BRLS_KBD_KEY_KP_4, "KP4"}, {brls::BRLS_KBD_KEY_KP_5, "KP5"},
            {brls::BRLS_KBD_KEY_KP_6, "KP6"}, {brls::BRLS_KBD_KEY_KP_7, "KP7"},
            {brls::BRLS_KBD_KEY_KP_8, "KP8"}, {brls::BRLS_KBD_KEY_KP_9, "KP9"},
            {brls::BRLS_KBD_KEY_KP_DECIMAL, "KPDECIMAL"},
            {brls::BRLS_KBD_KEY_KP_DIVIDE, "KPDIVIDE"},
            {brls::BRLS_KBD_KEY_KP_MULTIPLY, "KPMULTIPLY"},
            {brls::BRLS_KBD_KEY_KP_SUBTRACT, "KPSUBTRACT"},
            {brls::BRLS_KBD_KEY_KP_ADD, "KPADD"},
            {brls::BRLS_KBD_KEY_KP_EQUAL, "KPEQUAL"},
            {brls::BRLS_KBD_KEY_LEFT_SUPER, "LSUPER"},
            {brls::BRLS_KBD_KEY_RIGHT_SUPER, "RSUPER"},
            {brls::BRLS_KBD_KEY_MENU, "MENU"},
        };
#else
        const std::vector<CapturableKeyboardInput> kExtendedCapturableInputs;
#endif

        std::string uppercase(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::toupper(character));
                });
            return value;
        }

        int inputIdForToken(const std::string& token)
        {
            for (const auto& entry : beiklive::k_gameInputNames)
                if (token == entry.name)
                    return entry.id;
            return keyboardInputIdForName(uppercase(token));
        }
    }

    int keyboardInputIdForName(const std::string& name)
    {
        for (const auto& entry : beiklive::k_kbdInputNames)
            if (name == entry.name)
                return entry.id;
#if defined(__APPLE__) && !defined(__SWITCH__)
        for (const auto& entry : kExtendedKeyboardInputNames)
            if (name == entry.name)
                return entry.id;
#endif
        return -1;
    }

    const char* keyboardInputNameForId(int id)
    {
#if defined(__APPLE__) && !defined(__SWITCH__)
        for (const auto& entry : kExtendedKeyboardInputNames)
            if (entry.id == id)
                return entry.name;
#endif
        for (const auto& entry : beiklive::k_kbdInputNames)
            if (entry.id == id)
                return entry.name;
        switch (id)
        {
        case brls::BRLS_KBD_KEY_RIGHT_SHIFT:
            return "SHIFT";
        case brls::BRLS_KBD_KEY_RIGHT_CONTROL:
            return "CTRL";
        case brls::BRLS_KBD_KEY_RIGHT_ALT:
            return "ALT";
        default:
            break;
        }
        return nullptr;
    }

    const std::vector<CapturableKeyboardInput>& extendedCapturableKeyboardInputs()
    {
        return kExtendedCapturableInputs;
    }

    std::vector<std::vector<int>> parseMixedInputCombos(const std::string& value)
    {
        std::vector<std::vector<int>> result;
        std::istringstream combinations(value);
        std::string combination;
        while (std::getline(combinations, combination, '|'))
        {
            std::vector<int> parsed;
            std::istringstream tokens(combination);
            std::string token;
            bool valid = true;
            while (std::getline(tokens, token, '+'))
            {
                const size_t first = token.find_first_not_of(" \t");
                const size_t last = token.find_last_not_of(" \t");
                if (first == std::string::npos)
                {
                    valid = false;
                    break;
                }
                const int inputId = inputIdForToken(
                    token.substr(first, last - first + 1));
                if (inputId < 0)
                {
                    valid = false;
                    break;
                }
                parsed.push_back(inputId);
            }
            if (valid && !parsed.empty())
                result.push_back(std::move(parsed));
        }
        return result;
    }
}
