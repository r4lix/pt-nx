// Minimal SDL3 compatible layer for the Nintendo Switch (libnx). Implements only the API subset the P.T. port uses, so the engine
// code builds unchanged. Backed by hid (pads), audout (sound), nwindow (Vulkan surface). See sdl_switch.cpp.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int16_t Sint16;
typedef int32_t Sint32;

#define SDL_VERSIONNUM(major, minor, micro) ((major) * 1000000 + (minor) * 1000 + (micro))
#define SDL_VERSIONNUM_MAJOR(v) ((v) / 1000000)
#define SDL_VERSIONNUM_MINOR(v) (((v) / 1000) % 1000)
#define SDL_VERSIONNUM_MICRO(v) ((v) % 1000)

#ifdef __cplusplus
extern "C" {
#endif

void SDL_free(void* mem);
int SDL_GetSystemRAM(void);
const char* SDL_GetError(void);
int SDL_GetVersion(void);
bool SDL_SetHint(const char* name, const char* value);
#define SDL_HINT_JOYSTICK_HIDAPI "SDL_JOYSTICK_HIDAPI"
#define SDL_HINT_JOYSTICK_RAWINPUT "SDL_JOYSTICK_RAWINPUT"
#define SDL_HINT_JOYSTICK_DIRECTINPUT "SDL_JOYSTICK_DIRECTINPUT"
#define SDL_HINT_XINPUT_ENABLED "SDL_XINPUT_ENABLED"
#define SDL_HINT_JOYSTICK_WGI "SDL_JOYSTICK_WGI"
#define SDL_HINT_JOYSTICK_GAMEINPUT "SDL_JOYSTICK_GAMEINPUT"
#define SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"

typedef Uint32 SDL_InitFlags;
#define SDL_INIT_AUDIO 0x10u
#define SDL_INIT_VIDEO 0x20u
#define SDL_INIT_GAMEPAD 0x2000u
bool SDL_Init(SDL_InitFlags flags);
bool SDL_InitSubSystem(SDL_InitFlags flags);
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags);
void SDL_Quit(void);

/* time */
Uint64 SDL_GetTicks(void);
Uint64 SDL_GetTicksNS(void);
void SDL_Delay(Uint32 ms);
void SDL_DelayNS(Uint64 ns);
void SDL_DelayPrecise(Uint64 ns);

/* paths: sdmc:/switch/pt/ */
char* SDL_GetBasePath(void);
char* SDL_GetPrefPath(const char* org, const char* app);

/* window: one fixed full screen window, there is no desktop windowing on the console */
typedef struct SDL_Window SDL_Window;
typedef Uint64 SDL_WindowFlags;
typedef Uint32 SDL_DisplayID;
typedef struct SDL_DisplayMode {
    SDL_DisplayID displayID;
    Uint32 format;
    int w, h;
    float pixel_density;
    float refresh_rate;
} SDL_DisplayMode;
#define SDL_WINDOW_FULLSCREEN 0x1ull
#define SDL_WINDOW_VULKAN 0x10000000ull
#define SDL_WINDOW_RESIZABLE 0x20ull
#define SDL_WINDOW_MINIMIZED 0x40ull
#define SDL_WINDOW_INPUT_FOCUS 0x200ull
#define SDL_WINDOW_HIGH_PIXEL_DENSITY 0x2000ull
SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags);
void SDL_DestroyWindow(SDL_Window* window);
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* window);
bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h);
float SDL_GetWindowPixelDensity(SDL_Window* window);
bool SDL_SetWindowSize(SDL_Window* window, int w, int h);
bool SDL_SetWindowFullscreen(SDL_Window* window, bool fullscreen);
bool SDL_SetWindowFullscreenMode(SDL_Window* window, const SDL_DisplayMode* mode);
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window* window);
const SDL_DisplayMode* SDL_GetDesktopDisplayMode(SDL_DisplayID display);
bool SDL_SetWindowRelativeMouseMode(SDL_Window* window, bool enabled);

/* vulkan: VK_NN_vi_surface over the default NWindow (nxvk) */
struct VkInstance_T;
typedef struct VkInstance_T* VkInstance;
struct VkSurfaceKHR_T;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
struct VkAllocationCallbacks;
const char* const* SDL_Vulkan_GetInstanceExtensions(Uint32* count);
bool SDL_Vulkan_CreateSurface(SDL_Window* window, VkInstance instance, const struct VkAllocationCallbacks* allocator, VkSurfaceKHR* surface);

/* message box: the console has none, the text goes to the log */
#define SDL_MESSAGEBOX_ERROR 0x10u
bool SDL_ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message, SDL_Window* window);

/* scancodes (SDL3 values); the console has no keyboard, the table keeps the key binding code compiling */
typedef enum SDL_Scancode {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G,
    SDL_SCANCODE_H, SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_M, SDL_SCANCODE_N,
    SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T, SDL_SCANCODE_U,
    SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X, SDL_SCANCODE_Y, SDL_SCANCODE_Z,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,
    SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE = 41, SDL_SCANCODE_BACKSPACE = 42, SDL_SCANCODE_TAB = 43, SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7,
    SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_INSERT = 73, SDL_SCANCODE_PAGEUP = 75, SDL_SCANCODE_DELETE = 76, SDL_SCANCODE_PAGEDOWN = 78,
    SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80, SDL_SCANCODE_DOWN = 81, SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_KP_ENTER = 88,
    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT = 225, SDL_SCANCODE_LALT = 226,
    SDL_SCANCODE_RCTRL = 228, SDL_SCANCODE_RSHIFT = 229, SDL_SCANCODE_RALT = 230,
    SDL_SCANCODE_COUNT = 512
} SDL_Scancode;
typedef Uint32 SDL_Keycode;
typedef Uint16 SDL_Keymod;
#define SDLK_UNKNOWN 0u
#define SDL_KMOD_NONE 0u
#define SDL_KMOD_ALT 0x0300u
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode, SDL_Keymod modstate, bool key_event);
const char* SDL_GetKeyName(SDL_Keycode key);
const char* SDL_GetScancodeName(SDL_Scancode scancode);
SDL_Scancode SDL_GetScancodeFromName(const char* name);
const bool* SDL_GetKeyboardState(int* numkeys);

/* mouse: none on the console, the touch screen is not mapped */
typedef Uint32 SDL_MouseButtonFlags;
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
#define SDL_BUTTON_MASK(x) (1u << ((x) - 1))
#define SDL_BUTTON_LMASK SDL_BUTTON_MASK(SDL_BUTTON_LEFT)
#define SDL_BUTTON_MMASK SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)
#define SDL_BUTTON_RMASK SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)
SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y);
SDL_MouseButtonFlags SDL_GetRelativeMouseState(float* x, float* y);

/* properties (used for the gamepad rumble capabilities) */
typedef Uint32 SDL_PropertiesID;
#define SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN "SDL.gamepad.cap.rumble"
#define SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN "SDL.gamepad.cap.trigger_rumble"
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char* name, bool default_value);

/* gamepad: handheld and P1 through hid, one pad (id 1) */
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Joystick SDL_Joystick;
typedef Uint32 SDL_JoystickID;
typedef enum SDL_GamepadButton {
    SDL_GAMEPAD_BUTTON_INVALID = -1,
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_MISC1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, SDL_GAMEPAD_BUTTON_TOUCHPAD,
    SDL_GAMEPAD_BUTTON_COUNT
} SDL_GamepadButton;
typedef enum SDL_GamepadAxis {
    SDL_GAMEPAD_AXIS_INVALID = -1,
    SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_GAMEPAD_AXIS_COUNT
} SDL_GamepadAxis;
typedef enum SDL_GamepadType {
    SDL_GAMEPAD_TYPE_UNKNOWN = 0, SDL_GAMEPAD_TYPE_STANDARD, SDL_GAMEPAD_TYPE_XBOX360, SDL_GAMEPAD_TYPE_XBOXONE,
    SDL_GAMEPAD_TYPE_PS3, SDL_GAMEPAD_TYPE_PS4, SDL_GAMEPAD_TYPE_PS5, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,
    SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT, SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT,
    SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
} SDL_GamepadType;
typedef enum SDL_GamepadButtonLabel {
    SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN, SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_B,
    SDL_GAMEPAD_BUTTON_LABEL_X, SDL_GAMEPAD_BUTTON_LABEL_Y
} SDL_GamepadButtonLabel;
typedef enum SDL_PowerState {
    SDL_POWERSTATE_ERROR = -1, SDL_POWERSTATE_UNKNOWN, SDL_POWERSTATE_ON_BATTERY, SDL_POWERSTATE_NO_BATTERY,
    SDL_POWERSTATE_CHARGING, SDL_POWERSTATE_CHARGED
} SDL_PowerState;
typedef enum SDL_JoystickConnectionState {
    SDL_JOYSTICK_CONNECTION_INVALID = -1, SDL_JOYSTICK_CONNECTION_UNKNOWN, SDL_JOYSTICK_CONNECTION_WIRED,
    SDL_JOYSTICK_CONNECTION_WIRELESS
} SDL_JoystickConnectionState;
typedef enum SDL_JoystickType { SDL_JOYSTICK_TYPE_UNKNOWN, SDL_JOYSTICK_TYPE_GAMEPAD } SDL_JoystickType;

SDL_JoystickID* SDL_GetGamepads(int* count);
SDL_JoystickID* SDL_GetJoysticks(int* count);
bool SDL_IsGamepad(SDL_JoystickID id);
bool SDL_IsJoystickVirtual(SDL_JoystickID id);
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id);
void SDL_CloseGamepad(SDL_Gamepad* pad);
SDL_Joystick* SDL_OpenJoystick(SDL_JoystickID id);
void SDL_CloseJoystick(SDL_Joystick* joystick);
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad* pad);
const char* SDL_GetGamepadName(SDL_Gamepad* pad);
const char* SDL_GetGamepadPath(SDL_Gamepad* pad);
const char* SDL_GetGamepadSerial(SDL_Gamepad* pad);
char* SDL_GetGamepadMapping(SDL_Gamepad* pad);
Uint16 SDL_GetGamepadVendor(SDL_Gamepad* pad);
Uint16 SDL_GetGamepadProduct(SDL_Gamepad* pad);
int SDL_GetGamepadPlayerIndex(SDL_Gamepad* pad);
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad* pad);
const char* SDL_GetGamepadStringForType(SDL_GamepadType type);
SDL_GamepadButtonLabel SDL_GetGamepadButtonLabelForType(SDL_GamepadType type, SDL_GamepadButton button);
SDL_JoystickConnectionState SDL_GetGamepadConnectionState(SDL_Gamepad* pad);
SDL_PowerState SDL_GetGamepadPowerInfo(SDL_Gamepad* pad, int* percent);
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad* pad);
bool SDL_GetGamepadButton(SDL_Gamepad* pad, SDL_GamepadButton button);
Sint16 SDL_GetGamepadAxis(SDL_Gamepad* pad, SDL_GamepadAxis axis);
int SDL_GetNumGamepadTouchpads(SDL_Gamepad* pad);
int SDL_GetNumGamepadTouchpadFingers(SDL_Gamepad* pad, int touchpad);
bool SDL_GetGamepadTouchpadFinger(SDL_Gamepad* pad, int touchpad, int finger, bool* down, float* x, float* y, float* pressure);
bool SDL_RumbleGamepad(SDL_Gamepad* pad, Uint16 low_frequency, Uint16 high_frequency, Uint32 duration_ms);
const char* SDL_GetJoystickNameForID(SDL_JoystickID id);
const char* SDL_GetJoystickPathForID(SDL_JoystickID id);
Uint16 SDL_GetJoystickVendorForID(SDL_JoystickID id);
Uint16 SDL_GetJoystickProductForID(SDL_JoystickID id);

/* events: pads (added/removed, buttons, axes) and the system quit */
typedef enum SDL_EventType {
    SDL_EVENT_QUIT = 0x100,
    SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED = 0x203,
    SDL_EVENT_WINDOW_CLOSE_REQUESTED = 0x210,
    SDL_EVENT_KEY_DOWN = 0x300, SDL_EVENT_KEY_UP,
    SDL_EVENT_MOUSE_MOTION = 0x400, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP, SDL_EVENT_MOUSE_WHEEL,
    SDL_EVENT_GAMEPAD_AXIS_MOTION = 0x650, SDL_EVENT_GAMEPAD_BUTTON_DOWN = 0x651, SDL_EVENT_GAMEPAD_BUTTON_UP = 0x652,
    SDL_EVENT_GAMEPAD_ADDED = 0x653, SDL_EVENT_GAMEPAD_REMOVED = 0x654
} SDL_EventType;
typedef struct SDL_CommonEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; } SDL_CommonEvent;
typedef struct SDL_WindowEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 windowID; Sint32 data1; Sint32 data2; } SDL_WindowEvent;
typedef struct SDL_KeyboardEvent {
    Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 windowID; Uint32 which;
    SDL_Scancode scancode; SDL_Keycode key; SDL_Keymod mod; Uint16 raw; bool down; bool repeat;
} SDL_KeyboardEvent;
typedef struct SDL_MouseMotionEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 windowID; Uint32 which; SDL_MouseButtonFlags state; float x, y, xrel, yrel; } SDL_MouseMotionEvent;
typedef struct SDL_MouseButtonEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 windowID; Uint32 which; Uint8 button; bool down; Uint8 clicks; Uint8 padding; float x, y; } SDL_MouseButtonEvent;
typedef struct SDL_MouseWheelEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint32 windowID; Uint32 which; float x, y; Sint32 direction; float mouse_x, mouse_y; } SDL_MouseWheelEvent;
typedef struct SDL_GamepadDeviceEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_JoystickID which; } SDL_GamepadDeviceEvent;
typedef struct SDL_GamepadAxisEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_JoystickID which; Uint8 axis; Uint8 padding1, padding2, padding3; Sint16 value; Uint16 padding4; } SDL_GamepadAxisEvent;
typedef struct SDL_GamepadButtonEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_JoystickID which; Uint8 button; bool down; Uint8 padding1, padding2; } SDL_GamepadButtonEvent;
typedef union SDL_Event {
    Uint32 type;
    SDL_CommonEvent common;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_GamepadDeviceEvent gdevice;
    SDL_GamepadAxisEvent gaxis;
    SDL_GamepadButtonEvent gbutton;
    Uint8 padding[128];
} SDL_Event;
bool SDL_PollEvent(SDL_Event* event);

/* audio: playback only, through audout; the callback model of SDL_OpenAudioDeviceStream is kept */
typedef Uint32 SDL_AudioDeviceID;
typedef Uint32 SDL_AudioFormat;
#define SDL_AUDIO_S16 0x8010u
#define SDL_AUDIO_F32 0x8120u
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFFu)
typedef struct SDL_AudioSpec { SDL_AudioFormat format; int channels; int freq; } SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef void (*SDL_AudioStreamCallback)(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback, void* userdata);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream);
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len);
int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream);
int SDL_GetAudioStreamQueued(SDL_AudioStream* stream);
int SDL_GetAudioStreamData(SDL_AudioStream* stream, void* buf, int len);
bool SDL_ClearAudioStream(SDL_AudioStream* stream);
void SDL_DestroyAudioStream(SDL_AudioStream* stream);
/* recording belongs to the voice recognizer, which is not part of the console build */
SDL_AudioDeviceID* SDL_GetAudioRecordingDevices(int* count);
const char* SDL_GetAudioDeviceName(SDL_AudioDeviceID devid);
bool SDL_LoadWAV(const char* path, SDL_AudioSpec* spec, Uint8** audio_buf, Uint32* audio_len);
bool SDL_ConvertAudioSamples(const SDL_AudioSpec* src_spec, const Uint8* src_data, int src_len, const SDL_AudioSpec* dst_spec, Uint8** dst_data, int* dst_len);

/* child processes: the desktop tools (museum and loop preview generators) start one, a console application cannot */
typedef struct SDL_Process SDL_Process;
#define SDL_PROP_PROCESS_CREATE_ARGS_POINTER "SDL.process.create.args"
#define SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING "SDL.process.create.working_directory"
#define SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN "SDL.process.create.background"
SDL_PropertiesID SDL_CreateProperties(void);
void SDL_DestroyProperties(SDL_PropertiesID props);
bool SDL_SetPointerProperty(SDL_PropertiesID props, const char* name, void* value);
bool SDL_SetStringProperty(SDL_PropertiesID props, const char* name, const char* value);
bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char* name, bool value);
SDL_Process* SDL_CreateProcessWithProperties(SDL_PropertiesID props);
bool SDL_WaitProcess(SDL_Process* process, bool block, int* exitcode);
void SDL_DestroyProcess(SDL_Process* process);

#ifdef __cplusplus
}
#endif
