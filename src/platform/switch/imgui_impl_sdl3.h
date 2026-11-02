// Nintendo Switch replacement for Dear ImGui's SDL3 platform backend (backends/imgui_impl_sdl3.h): the same five entry points
// the engine calls, built on the SDL3 compatible layer. Display size, frame time and gamepad navigation; no mouse or keyboard.
#pragma once

#include <imgui.h>

#include <SDL3/SDL.h>

enum ImGui_ImplSDL3_GamepadMode { ImGui_ImplSDL3_GamepadMode_AutoFirst, ImGui_ImplSDL3_GamepadMode_AutoAll, ImGui_ImplSDL3_GamepadMode_Manual };

namespace pt_imgui_switch {
struct State {
    SDL_Window* window = nullptr;
    Uint64 last_ns = 0;
};
inline State& Get() {
    static State state;
    return state;
}
}  // namespace pt_imgui_switch

inline bool ImGui_ImplSDL3_InitForVulkan(SDL_Window* window) {
    pt_imgui_switch::Get().window = window;
    pt_imgui_switch::Get().last_ns = SDL_GetTicksNS();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "imgui_impl_sdl3_switch";
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    return true;
}

inline void ImGui_ImplSDL3_Shutdown() {
    pt_imgui_switch::Get() = {};
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
}

inline bool ImGui_ImplSDL3_ProcessEvent(const SDL_Event*) { return false; }

inline void ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode, SDL_Gamepad**, int) {}
inline void ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode) {}

inline void ImGui_ImplSDL3_NewFrame() {
    ImGuiIO& io = ImGui::GetIO();
    pt_imgui_switch::State& state = pt_imgui_switch::Get();
    int w = 1280, h = 720;
    SDL_GetWindowSizeInPixels(state.window, &w, &h);
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    const Uint64 now = SDL_GetTicksNS();
    const double dt = static_cast<double>(now - state.last_ns) / 1e9;
    state.last_ns = now;
    io.DeltaTime = dt > 0.0 ? static_cast<float>(dt) : 1.0f / 60.0f;

    // gamepad navigation: the console's A is SDL's east, B south (Nintendo layout: A confirms, B cancels)
    SDL_Gamepad* pad = SDL_OpenGamepad(1);
    if (!pad) return;
    struct Map {
        ImGuiKey key;
        SDL_GamepadButton button;
    };
    static const Map buttons[] = {
        {ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START},          {ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK},
        // ImGui activates with FaceDown and cancels with FaceRight: south is A and east is B (sdl_switch.cpp reports the buttons by role)
        {ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST},        {ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST},
        {ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH},         {ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH},
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},   {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},       {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},     {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {ImGuiKey_GamepadL3, SDL_GAMEPAD_BUTTON_LEFT_STICK},        {ImGuiKey_GamepadR3, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    };
    for (const Map& m : buttons) io.AddKeyEvent(m.key, SDL_GetGamepadButton(pad, m.button));
    auto axis = [&](ImGuiKey neg, ImGuiKey pos, SDL_GamepadAxis a) {
        const float v = static_cast<float>(SDL_GetGamepadAxis(pad, a)) / 32767.0f;
        io.AddKeyAnalogEvent(neg, v < -0.25f, v < -0.25f ? -v : 0.0f);
        io.AddKeyAnalogEvent(pos, v > 0.25f, v > 0.25f ? v : 0.0f);
    };
    axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX);
    axis(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000,
                         static_cast<float>(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) / 32767.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000,
                         static_cast<float>(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) / 32767.0f);
}
