#if defined(__APPLE__) && !defined(__SWITCH__)

#include <borealis/core/application.hpp>
#include <borealis/core/logger.hpp>
#include <borealis/platforms/glfw/glfw_input.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace beiklive
{
namespace
{

constexpr size_t kGamepadButtonCount = GLFW_GAMEPAD_BUTTON_LAST + 1;
constexpr size_t kGamepadAxisCount = GLFW_GAMEPAD_AXIS_LAST + 1;

constexpr std::array<size_t, kGamepadButtonCount> kButtonMapping = {
    brls::BUTTON_A,
    brls::BUTTON_B,
    brls::BUTTON_X,
    brls::BUTTON_Y,
    brls::BUTTON_LB,
    brls::BUTTON_RB,
    brls::BUTTON_BACK,
    brls::BUTTON_START,
    brls::BUTTON_GUIDE,
    brls::BUTTON_LSB,
    brls::BUTTON_RSB,
    brls::BUTTON_UP,
    brls::BUTTON_RIGHT,
    brls::BUTTON_DOWN,
    brls::BUTTON_LEFT,
};

constexpr std::array<size_t, kGamepadAxisCount> kAxisMapping = {
    brls::LEFT_X,
    brls::LEFT_Y,
    brls::RIGHT_X,
    brls::RIGHT_Y,
    brls::LEFT_Z,
    brls::RIGHT_Z,
};

bool isNintendoSwitchPro(int joystick)
{
    const char* guid = glfwGetJoystickGUID(joystick);
    if (guid && std::strncmp(guid, "030000007e0500000920000000000000", 32) == 0)
        return true;

    const char* name = glfwGetJoystickName(joystick);
    return name && (std::strstr(name, "Pro Controller") ||
                    std::strstr(name, "Nintendo Switch"));
}

bool readRawJoystickState(int joystick, GLFWgamepadstate* state)
{
    if (!state || !glfwJoystickPresent(joystick))
        return false;

    int axisCount = 0;
    int buttonCount = 0;
    int hatCount = 0;
    const float* axes = glfwGetJoystickAxes(joystick, &axisCount);
    const unsigned char* buttons = glfwGetJoystickButtons(joystick, &buttonCount);
    const unsigned char* hats = glfwGetJoystickHats(joystick, &hatCount);
    if (!axes && !buttons && !hats)
        return false;

    *state = {};
    if (isNintendoSwitchPro(joystick))
    {
        constexpr std::array<int, kGamepadButtonCount> switchButtons = {
            0, 1, 2, 3, 4, 5, 8, 9, 12, 10, 11,
            -1, -1, -1, -1,
        };
        for (size_t i = 0; i < switchButtons.size(); ++i)
        {
            const int raw = switchButtons[i];
            state->buttons[i] = raw >= 0 && raw < buttonCount && buttons[raw];
        }
        if (buttonCount > 6)
            state->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = buttons[6] ? 1.0f : 0.0f;
        if (buttonCount > 7)
            state->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = buttons[7] ? 1.0f : 0.0f;

        if (axisCount > 0) state->axes[GLFW_GAMEPAD_AXIS_LEFT_X] = axes[0];
        if (axisCount > 1) state->axes[GLFW_GAMEPAD_AXIS_LEFT_Y] = axes[1];
        if (axisCount > 2) state->axes[GLFW_GAMEPAD_AXIS_RIGHT_X] = axes[2];
        if (axisCount > 3) state->axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] = axes[3];
    }
    else
    {
        const int mappedButtons = std::min<int>(buttonCount, kGamepadButtonCount);
        for (int i = 0; i < mappedButtons; ++i)
            state->buttons[i] = buttons[i] != 0;

        const int mappedAxes = std::min<int>(axisCount, kGamepadAxisCount);
        const bool signedTriggers =
            (axisCount > GLFW_GAMEPAD_AXIS_LEFT_TRIGGER &&
             axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] < -0.25f) ||
            (axisCount > GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER &&
             axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] < -0.25f);
        for (int i = 0; i < mappedAxes; ++i)
        {
            const float value = axes[i];
            state->axes[i] = i >= GLFW_GAMEPAD_AXIS_LEFT_TRIGGER && signedTriggers
                ? (value + 1.0f) * 0.5f
                : value;
        }
    }

    if (hatCount > 0 && hats)
    {
        const unsigned char hat = hats[0];
        state->buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] |= (hat & GLFW_HAT_UP) != 0;
        state->buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] |= (hat & GLFW_HAT_RIGHT) != 0;
        state->buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] |= (hat & GLFW_HAT_DOWN) != 0;
        state->buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] |= (hat & GLFW_HAT_LEFT) != 0;
    }
    return true;
}

class GBAStationGLFWInputManager final : public brls::GLFWInputManager
{
  public:
    explicit GBAStationGLFWInputManager(GLFWwindow* window)
        : brls::GLFWInputManager(window), window(window)
    {
        active = this;
        syncConnectedJoysticks();
        glfwSetJoystickCallback(joystickCallback);
    }

    ~GBAStationGLFWInputManager() override
    {
        if (active == this)
            active = nullptr;
    }

    short getControllersConnectedCount() override
    {
        std::lock_guard<std::mutex> lock(joystickMutex);
        return static_cast<short>(connectedJoysticks.size());
    }

    void updateUnifiedControllerState(brls::ControllerState* state) override
    {
        if (!state)
            return;
        clearState(state);
        if (!glfwGetWindowAttrib(window, GLFW_FOCUSED))
            return;

        const int controllerCount = getControllersConnectedCount();
        for (int controller = 0; controller < controllerCount; ++controller)
        {
            brls::ControllerState local{};
            updateControllerState(&local, controller);
            for (size_t i = 0; i < brls::_BUTTON_MAX; ++i)
                state->buttons[i] |= local.buttons[i];
            for (size_t i = 0; i < brls::_AXES_MAX; ++i)
                state->axes[i] = std::clamp(state->axes[i] + local.axes[i], -1.0f, 1.0f);
        }

        const auto pressed = [this](int key) {
            return glfwGetKey(window, key) == GLFW_PRESS;
        };
        state->buttons[brls::BUTTON_X] |= pressed(GLFW_KEY_I);
        state->buttons[brls::BUTTON_Y] |= pressed(GLFW_KEY_J);
        state->buttons[brls::BUTTON_B] |= pressed(GLFW_KEY_K);
        state->buttons[brls::BUTTON_A] |= pressed(GLFW_KEY_L);
        state->buttons[brls::BUTTON_LB] |= pressed(GLFW_KEY_Q);
        state->buttons[brls::BUTTON_LT] |= pressed(GLFW_KEY_TAB);
        state->buttons[brls::BUTTON_RB] |= pressed(GLFW_KEY_O);
        state->buttons[brls::BUTTON_RT] |= pressed(GLFW_KEY_P);
        state->buttons[brls::BUTTON_LSB] |= pressed(GLFW_KEY_E);
        state->buttons[brls::BUTTON_RSB] |= pressed(GLFW_KEY_U);
        state->buttons[brls::BUTTON_START] |=
            pressed(GLFW_KEY_ENTER) || pressed(GLFW_KEY_KP_ENTER);
        state->buttons[brls::BUTTON_BACK] |= pressed(GLFW_KEY_BACKSPACE);
        state->buttons[brls::BUTTON_UP] |= pressed(GLFW_KEY_UP);
        state->buttons[brls::BUTTON_RIGHT] |= pressed(GLFW_KEY_RIGHT);
        state->buttons[brls::BUTTON_DOWN] |= pressed(GLFW_KEY_DOWN);
        state->buttons[brls::BUTTON_LEFT] |= pressed(GLFW_KEY_LEFT);

        const float keyboardLeftX = (pressed(GLFW_KEY_D) ? 1.0f : 0.0f) -
                                    (pressed(GLFW_KEY_A) ? 1.0f : 0.0f);
        const float keyboardLeftY = (pressed(GLFW_KEY_S) ? 1.0f : 0.0f) -
                                    (pressed(GLFW_KEY_W) ? 1.0f : 0.0f);
        state->axes[brls::LEFT_X] =
            std::clamp(state->axes[brls::LEFT_X] + keyboardLeftX, -1.0f, 1.0f);
        state->axes[brls::LEFT_Y] =
            std::clamp(state->axes[brls::LEFT_Y] + keyboardLeftY, -1.0f, 1.0f);

        state->buttons[brls::BUTTON_NAV_UP] |=
            keyboardLeftY < 0.0f || state->buttons[brls::BUTTON_UP];
        state->buttons[brls::BUTTON_NAV_RIGHT] |=
            keyboardLeftX > 0.0f || state->buttons[brls::BUTTON_RIGHT];
        state->buttons[brls::BUTTON_NAV_DOWN] |=
            keyboardLeftY > 0.0f || state->buttons[brls::BUTTON_DOWN];
        state->buttons[brls::BUTTON_NAV_LEFT] |=
            keyboardLeftX < 0.0f || state->buttons[brls::BUTTON_LEFT];
    }

    void updateControllerState(brls::ControllerState* state, int controller) override
    {
        if (!state)
            return;
        clearState(state);

        const int joystick = joystickIdForController(controller);
        if (joystick < 0)
            return;

        GLFWgamepadstate glfwState{};
        if (!glfwGetGamepadState(joystick, &glfwState) &&
            !readRawJoystickState(joystick, &glfwState))
            return;

        for (size_t i = 0; i < kButtonMapping.size(); ++i)
            state->buttons[kButtonMapping[i]] = glfwState.buttons[i] != 0;
        for (size_t i = 0; i < kAxisMapping.size(); ++i)
            state->axes[kAxisMapping[i]] = glfwState.axes[i];

        state->buttons[brls::BUTTON_LT] =
            glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.1f;
        state->buttons[brls::BUTTON_RT] =
            glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.1f;
        state->buttons[brls::BUTTON_NAV_UP] =
            glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] < -0.5f ||
            glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] < -0.5f ||
            state->buttons[brls::BUTTON_UP];
        state->buttons[brls::BUTTON_NAV_RIGHT] =
            glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_X] > 0.5f ||
            glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] > 0.5f ||
            state->buttons[brls::BUTTON_RIGHT];
        state->buttons[brls::BUTTON_NAV_DOWN] =
            glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] > 0.5f ||
            glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] > 0.5f ||
            state->buttons[brls::BUTTON_DOWN];
        state->buttons[brls::BUTTON_NAV_LEFT] =
            glfwState.axes[GLFW_GAMEPAD_AXIS_LEFT_X] < -0.5f ||
            glfwState.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] < -0.5f ||
            state->buttons[brls::BUTTON_LEFT];
    }

  private:
    static void clearState(brls::ControllerState* state)
    {
        std::fill(std::begin(state->buttons), std::end(state->buttons), false);
        std::fill(std::begin(state->axes), std::end(state->axes), 0.0f);
    }

    static void joystickCallback(int joystick, int event)
    {
        if (active)
            active->handleJoystickEvent(joystick, event);
        brls::Application::setActiveEvent(true);
    }

    void syncConnectedJoysticks()
    {
        std::vector<int> found;
        for (int joystick = 0; joystick <= GLFW_JOYSTICK_LAST; ++joystick)
        {
            if (!glfwJoystickPresent(joystick))
                continue;
            found.push_back(joystick);
            if (!glfwJoystickIsGamepad(joystick))
            {
                const char* name = glfwGetJoystickName(joystick);
                brls::Logger::info("glfw: joystick {} connected: \"{}\" (raw HID)",
                                   joystick, name ? name : "unknown");
            }
        }
        std::lock_guard<std::mutex> lock(joystickMutex);
        connectedJoysticks = std::move(found);
    }

    void handleJoystickEvent(int joystick, int event)
    {
        std::lock_guard<std::mutex> lock(joystickMutex);
        if (event == GLFW_CONNECTED)
        {
            if (std::find(connectedJoysticks.begin(), connectedJoysticks.end(), joystick) ==
                connectedJoysticks.end())
                connectedJoysticks.push_back(joystick);
            std::sort(connectedJoysticks.begin(), connectedJoysticks.end());
            const char* name = glfwGetJoystickName(joystick);
            brls::Logger::info("glfw: joystick {} connected: \"{}\"{}", joystick,
                name ? name : "unknown",
                glfwJoystickIsGamepad(joystick) ? " (mapped gamepad)" : " (raw HID)");
        }
        else if (event == GLFW_DISCONNECTED)
        {
            connectedJoysticks.erase(
                std::remove(connectedJoysticks.begin(), connectedJoysticks.end(), joystick),
                connectedJoysticks.end());
            brls::Logger::info("glfw: joystick {} disconnected", joystick);
        }
    }

    int joystickIdForController(int controller) const
    {
        std::lock_guard<std::mutex> lock(joystickMutex);
        if (controller < 0 || controller >= static_cast<int>(connectedJoysticks.size()))
            return -1;
        return connectedJoysticks[controller];
    }

    static GBAStationGLFWInputManager* active;
    GLFWwindow* window;
    mutable std::mutex joystickMutex;
    std::vector<int> connectedJoysticks;
};

GBAStationGLFWInputManager* GBAStationGLFWInputManager::active = nullptr;

} // namespace

brls::GLFWInputManager* createGBAStationGLFWInputManager(GLFWwindow* window)
{
    return new GBAStationGLFWInputManager(window);
}

} // namespace beiklive

#endif
