// SDL3 subset for the Nintendo Switch on libnx (see SDL3/SDL.h).
#include <SDL3/SDL.h>

#include "engine/core/log.h"

#include <netinet/in.h> /* struct in_addr, for __nxlink_host */
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

// nxvk is linked into the executable and has no loader: the surface function is asked from the driver itself. Weak so the
// engine's other code links (and fails at run time with a message) before the driver library is built.
extern "C" void* vk_icdGetInstanceProcAddr(VkInstance instance, const char* name) __attribute__((weak));

namespace {

std::string g_error;
SDL_InitFlags g_init = 0;
PadState g_pad;
bool g_pad_ready = false;
bool g_pad_announced = false;
std::deque<SDL_Event> g_events;
Uint64 g_pad_buttons_prev = 0;
Sint16 g_axis_prev[SDL_GAMEPAD_AXIS_COUNT] = {};
bool g_quit_sent = false;

constexpr SDL_JoystickID kPadId = 1;

struct WindowImpl {
    int w = 1280;
    int h = 720;
    bool fullscreen = true;
    SDL_DisplayMode mode{};
};
WindowImpl g_window;

void SetError(const char* text) { g_error = text; }

void PushEvent(const SDL_Event& event) { g_events.push_back(event); }

void EnsurePad() {
    if (g_pad_ready) return;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    g_pad_ready = true;
}

struct ButtonMap {
    SDL_GamepadButton sdl;
    Uint64 hid;
};
// The face buttons by role, not by position: the engine (a PS4 game) confirms and interacts with the south button (Cross) and goes back
// with the east one (Circle). On the Switch A confirms and B goes back (the console's own convention), so A is reported as south and B as
// east, and the labels below follow, so the prompts show A and B where they act. X and Y keep their places (north, west).
const ButtonMap kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, HidNpadButton_A},
    {SDL_GAMEPAD_BUTTON_EAST, HidNpadButton_B},
    {SDL_GAMEPAD_BUTTON_WEST, HidNpadButton_Y},
    {SDL_GAMEPAD_BUTTON_NORTH, HidNpadButton_X},
    {SDL_GAMEPAD_BUTTON_BACK, HidNpadButton_Minus},
    {SDL_GAMEPAD_BUTTON_START, HidNpadButton_Plus},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, HidNpadButton_StickL},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, HidNpadButton_StickR},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, HidNpadButton_L},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, HidNpadButton_R},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, HidNpadButton_Up},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, HidNpadButton_Down},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, HidNpadButton_Left},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, HidNpadButton_Right},
};

Sint16 StickAxis(int value) { return static_cast<Sint16>(std::clamp(value, -32767, 32767)); }

// test only: PT_AUTOWALK=<start s>,<walk s>,<turn -1|1>,<turn s> holds the left stick forward, then the right stick sideways, so that a run
// on a console without a person at it reaches the heavy part of the first room (set through romfs:/test_env.txt)
Sint16 AutoWalk(SDL_GamepadAxis axis) {
    static float start = -1.0f, walk = 0.0f, turn_sign = 0.0f, turn = 0.0f;
    static bool parsed = false;
    static const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    if (!parsed) {
        parsed = true;
        if (const char* env = std::getenv("PT_AUTOWALK")) {
            std::sscanf(env, "%f,%f,%f,%f", &start, &walk, &turn_sign, &turn);
        }
    }
    if (start < 0.0f) return 0;
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    if (axis == SDL_GAMEPAD_AXIS_LEFTY && t >= start && t < start + walk) return -32767;
    if (axis == SDL_GAMEPAD_AXIS_RIGHTX && t >= start + walk && t < start + walk + turn) return static_cast<Sint16>(turn_sign * 32767.0f);
    return 0;
}

Sint16 AxisValue(SDL_GamepadAxis axis) {
    const Uint64 down = padGetButtons(&g_pad);
    if (const Sint16 scripted = AutoWalk(axis); scripted != 0) return scripted;
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return StickAxis(padGetStickPos(&g_pad, 0).x);
    case SDL_GAMEPAD_AXIS_LEFTY: return StickAxis(-padGetStickPos(&g_pad, 0).y);
    case SDL_GAMEPAD_AXIS_RIGHTX: return StickAxis(padGetStickPos(&g_pad, 1).x);
    case SDL_GAMEPAD_AXIS_RIGHTY: return StickAxis(-padGetStickPos(&g_pad, 1).y);
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return (down & HidNpadButton_ZL) ? 32767 : 0;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return (down & HidNpadButton_ZR) ? 32767 : 0;
    default: return 0;
    }
}

void PumpPad() {
    EnsurePad();
    if (!g_pad_announced) {
        g_pad_announced = true;
        SDL_Event added{};
        added.type = SDL_EVENT_GAMEPAD_ADDED;
        added.gdevice.which = kPadId;
        PushEvent(added);
    }
    padUpdate(&g_pad);
    const Uint64 now = padGetButtons(&g_pad);
    for (const ButtonMap& map : kButtons) {
        const bool was = (g_pad_buttons_prev & map.hid) != 0;
        const bool is = (now & map.hid) != 0;
        if (was == is) continue;
        SDL_Event event{};
        event.type = is ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        event.gbutton.which = kPadId;
        event.gbutton.button = static_cast<Uint8>(map.sdl);
        event.gbutton.down = is;
        PushEvent(event);
    }
    g_pad_buttons_prev = now;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) {
        const Sint16 value = AxisValue(static_cast<SDL_GamepadAxis>(a));
        if (value == g_axis_prev[a]) continue;
        g_axis_prev[a] = value;
        SDL_Event event{};
        event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        event.gaxis.which = kPadId;
        event.gaxis.axis = static_cast<Uint8>(a);
        event.gaxis.value = value;
        PushEvent(event);
    }
}

// audio ------------------------------------------------------------------------------------------------------------

constexpr u32 kAudioFrames = 1024;
constexpr int kAudioBuffers = 3;
constexpr size_t kAudioBytes = (kAudioFrames * 2 * sizeof(s16) + 0xFFF) & ~size_t(0xFFF);

}  // namespace

struct SDL_Window {
    int unused = 0;
};

struct SDL_Gamepad {
    int unused = 0;
};

struct SDL_Joystick {
    int unused = 0;
};

struct SDL_AudioStream {
    SDL_AudioStreamCallback callback = nullptr;
    void* user = nullptr;
    int freq = 48000;
    int channels = 2;
    SDL_AudioFormat format = SDL_AUDIO_F32;
    std::mutex lock;
    std::vector<unsigned char> queue;
    std::atomic<bool> run{false};
    std::atomic<bool> stop{false};
    Thread thread{};
    bool thread_started = false;
    alignas(0x1000) unsigned char out[kAudioBuffers][kAudioBytes];
    AudioOutBuffer descriptors[kAudioBuffers]{};
};

namespace {

// renders one device buffer from the stream's queue (silence where the engine has not delivered yet) and hands it to audout
void FillAndQueue(SDL_AudioStream* stream, int index) {
    const int bytes_per_frame = stream->channels * (stream->format == SDL_AUDIO_F32 ? 4 : 2);
    const int want = static_cast<int>(kAudioFrames) * bytes_per_frame;
    if (stream->callback) stream->callback(stream->user, stream, want, want);
    s16* out = reinterpret_cast<s16*>(stream->out[index]);
    std::memset(out, 0, kAudioBytes);
    {
        std::lock_guard<std::mutex> guard(stream->lock);
        const size_t have_frames = stream->queue.size() / static_cast<size_t>(bytes_per_frame);
        const size_t frames = std::min<size_t>(have_frames, kAudioFrames);
        if (stream->format == SDL_AUDIO_F32) {
            const float* in = reinterpret_cast<const float*>(stream->queue.data());
            for (size_t i = 0; i < frames * 2; ++i) out[i] = static_cast<s16>(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f);
        } else {
            std::memcpy(out, stream->queue.data(), frames * 4);
        }
        stream->queue.erase(stream->queue.begin(), stream->queue.begin() + static_cast<std::ptrdiff_t>(frames * bytes_per_frame));
    }
    AudioOutBuffer& d = stream->descriptors[index];
    d.next = nullptr;
    d.buffer = stream->out[index];
    d.buffer_size = kAudioBytes;
    d.data_size = kAudioFrames * 2 * sizeof(s16);
    d.data_offset = 0;
    audoutAppendAudioOutBuffer(&d);
}

void AudioThread(void* arg) {
    auto* stream = static_cast<SDL_AudioStream*>(arg);
    while (!stream->run.load() && !stream->stop.load()) svcSleepThread(5'000'000);
    // all buffers go in first so the device never runs dry, then each one is refilled as audout releases it
    for (int i = 0; i < kAudioBuffers && !stream->stop.load(); ++i) FillAndQueue(stream, i);
    while (!stream->stop.load()) {
        AudioOutBuffer* released = nullptr;
        u32 released_count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &released_count, 50'000'000)) || released_count == 0) continue;
        for (u32 n = 0; released && n < released_count; ++n) {
            const int index = static_cast<int>(released - stream->descriptors);
            AudioOutBuffer* next = released->next;
            if (index >= 0 && index < kAudioBuffers) FillAndQueue(stream, index);
            released = next;
        }
    }
}

}  // namespace

extern "C" {

void SDL_free(void* mem) { std::free(mem); }
const char* SDL_GetError(void) { return g_error.c_str(); }
int SDL_GetVersion(void) { return SDL_VERSIONNUM(3, 4, 16); }
bool SDL_SetHint(const char*, const char*) { return true; }

bool SDL_Init(SDL_InitFlags flags) { return SDL_InitSubSystem(flags); }

bool SDL_InitSubSystem(SDL_InitFlags flags) {
    if ((flags & SDL_INIT_AUDIO) && !(g_init & SDL_INIT_AUDIO)) {
        pt::LogInfo("switch: audoutInitialize");
        const Result init = audoutInitialize();
        pt::LogInfo("switch: audoutInitialize -> {:#x}", static_cast<unsigned>(init));
        if (R_FAILED(init)) {
            SetError("audout init failed");
            return false;
        }
        const Result start = audoutStartAudioOut();
        pt::LogInfo("switch: audoutStartAudioOut -> {:#x}", static_cast<unsigned>(start));
        if (R_FAILED(start)) {
            SetError("audout start failed");
            return false;
        }
    }
    g_init |= flags;
    return true;
}

SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) { return g_init & flags; }

void SDL_Quit(void) {
    if (g_init & SDL_INIT_AUDIO) {
        audoutStopAudioOut();
        audoutExit();
    }
    g_init = 0;
}

Uint64 SDL_GetTicksNS(void) { return armTicksToNs(armGetSystemTick()); }
Uint64 SDL_GetTicks(void) { return SDL_GetTicksNS() / 1'000'000ull; }
void SDL_Delay(Uint32 ms) { svcSleepThread(static_cast<s64>(ms) * 1'000'000); }
void SDL_DelayNS(Uint64 ns) { svcSleepThread(static_cast<s64>(ns)); }
void SDL_DelayPrecise(Uint64 ns) { svcSleepThread(static_cast<s64>(ns)); }

char* SDL_GetBasePath(void) { return strdup("sdmc:/switch/pt/"); }

char* SDL_GetPrefPath(const char*, const char*) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/pt", 0777);
    mkdir("sdmc:/switch/pt/save", 0777);
    return strdup("sdmc:/switch/pt/save/");
}

SDL_Window* SDL_CreateWindow(const char*, int, int, SDL_WindowFlags) {
    static SDL_Window window;
    pt::LogInfo("switch: nwindowGetDefault");
    NWindow* native = nwindowGetDefault();
    pt::LogInfo("switch: window {}", static_cast<const void*>(native));
    // handheld renders 720p, docked 1080p
    const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    pt::LogInfo("switch: {}", docked ? "docked" : "handheld");
    g_window.w = docked ? 1920 : 1280;
    g_window.h = docked ? 1080 : 720;
    nwindowSetDimensions(native, static_cast<u32>(g_window.w), static_cast<u32>(g_window.h));
    g_window.mode = {1, 0, g_window.w, g_window.h, 1.0f, 60.0f};
    return &window;
}

void SDL_DestroyWindow(SDL_Window*) {}
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window*) { return SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN | SDL_WINDOW_INPUT_FOCUS; }

bool SDL_GetWindowSizeInPixels(SDL_Window*, int* w, int* h) {
    if (w) *w = g_window.w;
    if (h) *h = g_window.h;
    return true;
}

float SDL_GetWindowPixelDensity(SDL_Window*) { return 1.0f; }
bool SDL_SetWindowSize(SDL_Window*, int, int) { return true; }
bool SDL_SetWindowFullscreen(SDL_Window*, bool) { return true; }
bool SDL_SetWindowFullscreenMode(SDL_Window*, const SDL_DisplayMode*) { return true; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window*) { return 1; }
const SDL_DisplayMode* SDL_GetDesktopDisplayMode(SDL_DisplayID) { return &g_window.mode; }
bool SDL_SetWindowRelativeMouseMode(SDL_Window*, bool) { return true; }

const char* const* SDL_Vulkan_GetInstanceExtensions(Uint32* count) {
    static const char* const names[] = {"VK_KHR_surface", "VK_NN_vi_surface"};
    if (count) *count = 2;
    return names;
}

bool SDL_Vulkan_CreateSurface(SDL_Window*, VkInstance instance, const struct VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    if (!vk_icdGetInstanceProcAddr) {
        SetError("the nxvk Vulkan driver is not linked into this build");
        return false;
    }
    // VkViSurfaceCreateInfoNN (VK_NN_vi_surface), declared here so no extra Vulkan header is needed in the shim
    struct ViSurfaceCreateInfo {
        int32_t sType;
        const void* pNext;
        uint32_t flags;
        void* window;
    };
    using CreateFn = int32_t (*)(VkInstance, const ViSurfaceCreateInfo*, const struct VkAllocationCallbacks*, VkSurfaceKHR*);
    auto create = reinterpret_cast<CreateFn>(vk_icdGetInstanceProcAddr(instance, "vkCreateViSurfaceNN"));
    if (!create) {
        SetError("vkCreateViSurfaceNN is not available");
        return false;
    }
    const ViSurfaceCreateInfo info{1000062000, nullptr, 0, nwindowGetDefault()};
    if (create(instance, &info, allocator, surface) != 0) {
        SetError("vkCreateViSurfaceNN failed");
        return false;
    }
    return true;
}

bool SDL_ShowSimpleMessageBox(Uint32, const char* title, const char* message, SDL_Window*) {
    std::fprintf(stderr, "%s: %s\n", title ? title : "", message ? message : "");
    return true;
}

SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode, SDL_Keymod, bool) { return SDLK_UNKNOWN; }
const char* SDL_GetKeyName(SDL_Keycode) { return ""; }
const char* SDL_GetScancodeName(SDL_Scancode) { return ""; }
SDL_Scancode SDL_GetScancodeFromName(const char*) { return SDL_SCANCODE_UNKNOWN; }

const bool* SDL_GetKeyboardState(int* numkeys) {
    static const bool keys[SDL_SCANCODE_COUNT] = {};
    if (numkeys) *numkeys = SDL_SCANCODE_COUNT;
    return keys;
}

SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y) {
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}

SDL_MouseButtonFlags SDL_GetRelativeMouseState(float* x, float* y) { return SDL_GetMouseState(x, y); }

bool SDL_GetBooleanProperty(SDL_PropertiesID, const char* name, bool default_value) {
    // HD rumble is there on the Joy-Cons and the Pro Controller; the triggers have no motor
    if (name && std::strcmp(name, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN) == 0) return true;
    if (name && std::strcmp(name, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN) == 0) return false;
    return default_value;
}

SDL_JoystickID* SDL_GetGamepads(int* count) {
    auto* ids = static_cast<SDL_JoystickID*>(std::malloc(2 * sizeof(SDL_JoystickID)));
    ids[0] = kPadId;
    ids[1] = 0;
    if (count) *count = 1;
    return ids;
}

SDL_JoystickID* SDL_GetJoysticks(int* count) { return SDL_GetGamepads(count); }
bool SDL_IsGamepad(SDL_JoystickID id) { return id == kPadId; }
bool SDL_IsJoystickVirtual(SDL_JoystickID) { return false; }

SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id) {
    static SDL_Gamepad pad;
    return id == kPadId ? &pad : nullptr;
}

void SDL_CloseGamepad(SDL_Gamepad*) {}

SDL_Joystick* SDL_OpenJoystick(SDL_JoystickID id) {
    static SDL_Joystick joystick;
    return id == kPadId ? &joystick : nullptr;
}

void SDL_CloseJoystick(SDL_Joystick*) {}
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad*) { return kPadId; }
const char* SDL_GetGamepadName(SDL_Gamepad*) { return "Nintendo Switch Controller"; }
const char* SDL_GetGamepadPath(SDL_Gamepad*) { return "hid:npad:1"; }
const char* SDL_GetGamepadSerial(SDL_Gamepad*) { return ""; }
char* SDL_GetGamepadMapping(SDL_Gamepad*) { return nullptr; }
Uint16 SDL_GetGamepadVendor(SDL_Gamepad*) { return 0x057e; }
Uint16 SDL_GetGamepadProduct(SDL_Gamepad*) { return 0x2009; }
int SDL_GetGamepadPlayerIndex(SDL_Gamepad*) { return 0; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad*) { return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO; }

const char* SDL_GetGamepadStringForType(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "switchpro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "joyconleft";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "joyconright";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "joyconpair";
    default: return "standard";
    }
}

SDL_GamepadButtonLabel SDL_GetGamepadButtonLabelForType(SDL_GamepadType, SDL_GamepadButton button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return SDL_GAMEPAD_BUTTON_LABEL_A;  // see kButtons: A takes the confirm role
    case SDL_GAMEPAD_BUTTON_EAST: return SDL_GAMEPAD_BUTTON_LABEL_B;
    case SDL_GAMEPAD_BUTTON_WEST: return SDL_GAMEPAD_BUTTON_LABEL_Y;
    case SDL_GAMEPAD_BUTTON_NORTH: return SDL_GAMEPAD_BUTTON_LABEL_X;
    default: return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    }
}

SDL_JoystickConnectionState SDL_GetGamepadConnectionState(SDL_Gamepad*) { return SDL_JOYSTICK_CONNECTION_WIRELESS; }

SDL_PowerState SDL_GetGamepadPowerInfo(SDL_Gamepad*, int* percent) {
    if (percent) *percent = -1;
    return SDL_POWERSTATE_UNKNOWN;
}

SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad*) { return 1; }

bool SDL_GetGamepadButton(SDL_Gamepad*, SDL_GamepadButton button) {
    EnsurePad();
    for (const ButtonMap& map : kButtons) {
        if (map.sdl == button) return (padGetButtons(&g_pad) & map.hid) != 0;
    }
    return false;
}

Sint16 SDL_GetGamepadAxis(SDL_Gamepad*, SDL_GamepadAxis axis) {
    EnsurePad();
    return AxisValue(axis);
}

int SDL_GetNumGamepadTouchpads(SDL_Gamepad*) { return 0; }
int SDL_GetNumGamepadTouchpadFingers(SDL_Gamepad*, int) { return 0; }
bool SDL_GetGamepadTouchpadFinger(SDL_Gamepad*, int, int, bool*, float*, float*, float*) { return false; }

bool SDL_RumbleGamepad(SDL_Gamepad*, Uint16 low, Uint16 high, Uint32) {
    // HD rumble: a vibration value per hand, amplitude from the SDL motor strengths
    static HidVibrationDeviceHandle handles[2];
    static bool ready = false;
    if (!ready) {
        const HidNpadIdType id = HidNpadIdType_Handheld;
        hidInitializeVibrationDevices(&handles[0], 1, id, HidNpadStyleTag_NpadHandheld);
        hidInitializeVibrationDevices(&handles[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
        ready = true;
    }
    HidVibrationValue value{};
    value.freq_low = 160.0f;
    value.freq_high = 320.0f;
    value.amp_low = static_cast<float>(low) / 65535.0f;
    value.amp_high = static_cast<float>(high) / 65535.0f;
    hidSendVibrationValue(handles[0], &value);
    hidSendVibrationValue(handles[1], &value);
    return true;
}

const char* SDL_GetJoystickNameForID(SDL_JoystickID) { return "Nintendo Switch Controller"; }
const char* SDL_GetJoystickPathForID(SDL_JoystickID) { return "hid:npad:1"; }
Uint16 SDL_GetJoystickVendorForID(SDL_JoystickID) { return 0x057e; }
Uint16 SDL_GetJoystickProductForID(SDL_JoystickID) { return 0x2009; }

bool SDL_PollEvent(SDL_Event* event) {
    if (g_events.empty()) {
        // the home menu or a power button press ends the main loop
        if (!appletMainLoop() && !g_quit_sent) {
            g_quit_sent = true;
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            PushEvent(quit);
        }
        // test only (romfs:/test_env.txt): PT_QUIT_AFTER=<s> ends the run by itself; PT_NEXT_NRO=<sdmc path> has hbloader start that NRO
        // afterwards (the SwitchLink app, so the next run can be launched remotely)
        static const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        if (const char* after = std::getenv("PT_QUIT_AFTER"); after && !g_quit_sent &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= std::atof(after)) {
            g_quit_sent = true;
            if (const char* next = std::getenv("PT_NEXT_NRO"); next && envHasNextLoad()) {
                const std::string argv = std::string("\"") + next + "\"";
                const Result rc = envSetNextLoad(next, argv.c_str());
                pt::LogInfo("switch: test run over after {} s, next load {} ({})", after, next, R_SUCCEEDED(rc) ? "set" : "failed");
            } else {
                pt::LogInfo("switch: test run over after {} s", after);
            }
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            PushEvent(quit);
        }
        PumpPad();
    }
    if (g_events.empty()) return false;
    if (event) *event = g_events.front();
    g_events.pop_front();
    return true;
}

SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback, void* userdata) {
    if (!(g_init & SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return nullptr;
    if (!spec || spec->channels != 2 || (spec->format != SDL_AUDIO_F32 && spec->format != SDL_AUDIO_S16)) {
        SetError("only stereo F32 and S16 playback is supported");
        return nullptr;
    }
    if (spec->freq != 48000) {
        // audout plays 48 kHz only; another rate would play at the wrong pitch
        SetError("only 48000 Hz playback is supported");
        return nullptr;
    }
    auto* stream = new SDL_AudioStream();
    stream->callback = callback;
    stream->user = userdata;
    stream->freq = spec->freq;
    stream->channels = spec->channels;
    stream->format = spec->format;
    if (R_FAILED(threadCreate(&stream->thread, AudioThread, stream, nullptr, 0x40000, 0x2B, -2))) {
        delete stream;
        SetError("cannot start the audio thread");
        return nullptr;
    }
    stream->thread_started = true;
    threadStart(&stream->thread);
    return stream;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream) {
    if (!stream) return false;
    stream->run = true;
    return true;
}

bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len) {
    if (!stream || !buf || len <= 0) return false;
    std::lock_guard<std::mutex> guard(stream->lock);
    const auto* bytes = static_cast<const unsigned char*>(buf);
    stream->queue.insert(stream->queue.end(), bytes, bytes + len);
    return true;
}

int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream) {
    std::lock_guard<std::mutex> guard(stream->lock);
    return static_cast<int>(stream->queue.size());
}

int SDL_GetAudioStreamQueued(SDL_AudioStream* stream) { return SDL_GetAudioStreamAvailable(stream); }

int SDL_GetAudioStreamData(SDL_AudioStream* stream, void* buf, int len) {
    std::lock_guard<std::mutex> guard(stream->lock);
    const int n = std::min<int>(len, static_cast<int>(stream->queue.size()));
    std::memcpy(buf, stream->queue.data(), static_cast<size_t>(n));
    stream->queue.erase(stream->queue.begin(), stream->queue.begin() + n);
    return n;
}

bool SDL_ClearAudioStream(SDL_AudioStream* stream) {
    std::lock_guard<std::mutex> guard(stream->lock);
    stream->queue.clear();
    return true;
}

void SDL_DestroyAudioStream(SDL_AudioStream* stream) {
    if (!stream) return;
    stream->stop = true;
    if (stream->thread_started) {
        threadWaitForExit(&stream->thread);
        threadClose(&stream->thread);
    }
    delete stream;
}

SDL_AudioDeviceID* SDL_GetAudioRecordingDevices(int* count) {
    if (count) *count = 0;
    return nullptr;
}

const char* SDL_GetAudioDeviceName(SDL_AudioDeviceID) { return nullptr; }

bool SDL_LoadWAV(const char*, SDL_AudioSpec*, Uint8**, Uint32*) {
    SetError("WAV loading is not available on this platform");
    return false;
}

bool SDL_ConvertAudioSamples(const SDL_AudioSpec*, const Uint8*, int, const SDL_AudioSpec*, Uint8**, int*) {
    SetError("audio conversion is not available on this platform");
    return false;
}

SDL_PropertiesID SDL_CreateProperties(void) { return 1; }
void SDL_DestroyProperties(SDL_PropertiesID) {}
bool SDL_SetPointerProperty(SDL_PropertiesID, const char*, void*) { return true; }
bool SDL_SetStringProperty(SDL_PropertiesID, const char*, const char*) { return true; }
bool SDL_SetBooleanProperty(SDL_PropertiesID, const char*, bool) { return true; }

SDL_Process* SDL_CreateProcessWithProperties(SDL_PropertiesID) {
    SetError("a console application cannot start another process");
    return nullptr;
}

bool SDL_WaitProcess(SDL_Process*, bool, int* exitcode) {
    if (exitcode) *exitcode = -1;
    return true;
}

void SDL_DestroyProcess(SDL_Process*) {}

}  // extern "C"

// nxvk's requirement ("not to OOM immediately", switch/README.md in nxvk): the process is an Application and takes the whole heap.
extern "C" {
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
}

// libnx calls these before main and after it. romfs: holds the shaders and fonts built into the .nro. When the .nro was started with
// nxlink (`nxlink -s`), stdout and stderr, and with them the engine's log, stream to the PC; started from the menu nothing is set up.
static bool g_socket_up = false;

extern "C" void userAppInit(void) {
    romfsInit();
    if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault())) {
        g_socket_up = true;
        nxlinkStdio();
    }
}

extern "C" void pt_switch_release_gpu_clock();
extern "C" void pt_switch_release_mem_clock();
extern "C" void pt_switch_release_cpu_clock();
extern "C" unsigned pt_switch_cpu_rates(unsigned* out_mhz, unsigned max);
extern "C" void pt_switch_close_clock_services();

extern "C" void userAppExit(void) {
    pt_switch_release_gpu_clock();
    pt_switch_release_mem_clock();
    pt_switch_release_cpu_clock();
    pt_switch_close_clock_services();  // after the releases above, which go through the clock manager's session
    if (g_socket_up) socketExit();
    romfsExit();
}

extern "C" int SDL_GetSystemRAM(void) {
    // the memory this process may use: about 3.2 GB when started in title takeover mode (R held), a fraction of it in applet mode
    u64 total = 0;
    if (R_FAILED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) return 0;
    return static_cast<int>(total / (1024 * 1024));
}

// A worker thread with its own stack (the renderer builds pipelines in one: the driver's compiler needs more than a default thread stack), on
// core 1 so it does not take turns with the main thread, at a slightly lower priority. Null when no thread can be made.
extern "C" void* pt_switch_thread_start(void (*entry)(void*), void* arg, size_t stack_bytes) {
    auto* thread = new Thread{};
    if (R_FAILED(threadCreate(thread, entry, arg, nullptr, stack_bytes, 0x2D, 1)) && R_FAILED(threadCreate(thread, entry, arg, nullptr, stack_bytes, 0x2D, -2))) {
        delete thread;
        return nullptr;
    }
    if (R_FAILED(threadStart(thread))) {
        threadClose(thread);
        delete thread;
        return nullptr;
    }
    return thread;
}

extern "C" void pt_switch_thread_join(void* handle) {
    auto* thread = static_cast<Thread*>(handle);
    threadWaitForExit(thread);
    threadClose(thread);
    delete thread;
}

// Read-only clock report for the log (the clkrst service on firmware 8+, pcv before). Never sets a clock.
namespace {
int g_clk_state = 0;  // 0 untried, 1 clkrst, 2 pcv, -1 none
ClkrstSession g_clk_cpu, g_clk_gpu, g_clk_emc;
bool g_clk_emc_ok = false;  // the memory controller (EMC) clock is readable

// Every session opened here is closed again at exit (pt_switch_close_clock_services). A .nro started from the homebrew menu or chained to the
// next one runs inside the same process (hbloader), so sessions it leaves open stay open: after a few launches in a row pcv, which serves
// clkrst, ran out of session memory and aborted (fatal 2011-0102, program 010000000000001A, Switch Lite).
bool EnsureClocks() {
    if (g_clk_state != 0) return g_clk_state > 0;
    g_clk_state = -1;
    if (R_SUCCEEDED(clkrstInitialize())) {
        if (R_SUCCEEDED(clkrstOpenSession(&g_clk_cpu, PcvModuleId_CpuBus, 3))) {
            if (R_SUCCEEDED(clkrstOpenSession(&g_clk_gpu, PcvModuleId_GPU, 3))) {
                g_clk_state = 1;
                g_clk_emc_ok = R_SUCCEEDED(clkrstOpenSession(&g_clk_emc, PcvModuleId_EMC, 3));
            } else {
                clkrstCloseSession(&g_clk_cpu);
            }
        }
        if (g_clk_state != 1) clkrstExit();
    }
    if (g_clk_state != 1 && R_SUCCEEDED(pcvInitialize())) {
        g_clk_state = 2;
        g_clk_emc_ok = true;
    }
    return g_clk_state > 0;
}

// A clock manager (Horizon OC's hoc-clk, or sys-clk) puts its own clock back every tick, so a clock set through clkrst alone does not
// last. When one runs, the game asks it for a temporary override instead (what its overlay does), and clears it on exit.
Service g_clock_manager;
int g_clock_manager_state = 0;  // 0 untried, 1 connected, -1 none
bool g_clock_override_set[3] = {};  // per module: 0 CPU, 1 GPU, 2 memory

bool EnsureClockManager() {
    if (g_clock_manager_state != 0) return g_clock_manager_state > 0;
    g_clock_manager_state = -1;
    for (const char* name : {"hoc:clk", "sysclk:ipc"}) {
        if (R_SUCCEEDED(smGetService(&g_clock_manager, name))) {
            g_clock_manager_state = 1;
            pt::LogInfo("clocks: clock manager '{}' found, the GPU clock goes through its override", name);
            break;
        }
    }
    return g_clock_manager_state > 0;
}

// hoc-clk / sys-clk command 8 (SetOverride): module 0 = CPU, 1 = GPU, 2 = memory; hz 0 = no override
bool ClockManagerOverride(u32 module, u32 hz) {
    if (module > 2 || !EnsureClockManager()) return false;
    struct {
        u32 module;
        u32 hz;
    } args = {module, hz};
    const Result rc = serviceDispatchIn(&g_clock_manager, 8, args);
    if (R_FAILED(rc)) {
        pt::LogInfo("clocks: the clock manager refused the {} override ({:#x})", module == 2 ? "memory" : (module == 1 ? "GPU" : "CPU"),
                    static_cast<unsigned>(rc));
        return false;
    }
    g_clock_override_set[module] = hz != 0;
    return true;
}
bool ClockManagerOverrideGpu(u32 hz) { return ClockManagerOverride(1u, hz); }

// the allowed rates of a module in Hz (discrete lists only); returns how many
s32 PossibleRates(ClkrstSession* session, PcvModule module, u32* rates, s32 max) {
    s32 count = 0;
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
    const Result rc = g_clk_state == 1 ? clkrstGetPossibleClockRates(session, rates, max, &type, &count)
                                        : pcvGetPossibleClockRates(module, rates, max, &type, &count);
    if (R_FAILED(rc) || type != PcvClockRatesListType_Discrete) return 0;
    return std::min(count, max);
}
}  // namespace

extern "C" bool pt_switch_has_clock_manager() { return EnsureClockManager(); }

// test runs (PT_QUIT_AFTER) have nobody at the console: no auto-sleep while this application runs (the setting belongs to the application,
// so it ends with it; a battery too low to stay awake still sends the console to sleep)
extern "C" void pt_switch_disable_auto_sleep() {
    const Result rc = appletSetAutoSleepDisabled(true);
    pt::LogInfo("switch: auto-sleep off for this test run ({:#x})", static_cast<unsigned>(rc));
}

// hands the GPU clock back to the clock manager's own setting (the menu's "System", and at exit)
extern "C" void pt_switch_release_gpu_clock() {
    if (g_clock_override_set[1]) ClockManagerOverrideGpu(0);
}

// The memory controller (EMC) clock. It sets the GPU's memory bandwidth (the GPU shares the console's LPDDR4); handheld mode runs it at
// 1331 MHz, docked at 1600 MHz, and an overclocking KIP (Horizon OC) can allow more.
extern "C" unsigned pt_switch_mem_hz() {
    if (!EnsureClocks() || !g_clk_emc_ok) return 0;
    u32 hz = 0;
    if (R_FAILED(g_clk_state == 1 ? clkrstGetClockRate(&g_clk_emc, &hz) : pcvGetClockRate(PcvModule_EMC, &hz))) return 0;
    return hz;
}

// the allowed memory rates in MHz, ascending; returns how many
extern "C" unsigned pt_switch_mem_rates(unsigned* out_mhz, unsigned max) {
    if (!EnsureClocks() || !g_clk_emc_ok) return 0;
    u32 rates[40] = {};
    const s32 count = PossibleRates(&g_clk_emc, PcvModule_EMC, rates, 40);
    unsigned n = 0;
    for (s32 i = 0; i < count && n < max; ++i) out_mhz[n++] = rates[i] / 1000000u;
    return n;
}

// Requests a memory clock: the highest allowed rate at or below `mhz`. With a clock manager through its override only (as for the GPU);
// without one through clkrst. Returns the requested rate in MHz, 0 on failure.
extern "C" unsigned pt_switch_set_mem_mhz(unsigned mhz) {
    if (!EnsureClocks() || !g_clk_emc_ok) return 0;
    u32 rates[40] = {};
    const s32 count = PossibleRates(&g_clk_emc, PcvModule_EMC, rates, 40);
    if (count <= 0) return 0;
    u32 pick = rates[0];
    for (s32 i = 0; i < count; ++i) {
        if (rates[i] / 1000000u <= mhz && rates[i] > pick) pick = rates[i];
    }
    if (ClockManagerOverride(2u, pick)) return pick / 1000000u;
    const Result set = g_clk_state == 1 ? clkrstSetClockRate(&g_clk_emc, pick) : pcvSetClockRate(PcvModule_EMC, pick);
    if (R_FAILED(set)) {
        pt::LogInfo("clocks: setting the memory to {} MHz failed ({:#x})", pick / 1000000, static_cast<unsigned>(set));
        return 0;
    }
    return pick / 1000000u;
}

extern "C" void pt_switch_release_mem_clock() {
    if (g_clock_override_set[2]) ClockManagerOverride(2u, 0);
}

// at exit: the clkrst / pcv sessions and the clock manager's session (see EnsureClocks for why they must not outlive the .nro)
extern "C" void pt_switch_close_clock_services() {
    if (g_clk_state == 1) {
        if (g_clk_emc_ok) clkrstCloseSession(&g_clk_emc);
        clkrstCloseSession(&g_clk_gpu);
        clkrstCloseSession(&g_clk_cpu);
        clkrstExit();
    } else if (g_clk_state == 2) {
        pcvExit();
    }
    g_clk_state = -1;
    g_clk_emc_ok = false;
    if (g_clock_manager_state > 0) {
        serviceClose(&g_clock_manager);
        g_clock_manager_state = -1;
    }
}

// test runs: put the memory clock back to what it was before them (an override someone set in the clock manager's overlay stays set:
// the rate is requested again and left in place at exit; the handheld default 1331 MHz is simply released)
extern "C" void pt_switch_restore_mem_mhz(unsigned mhz) {
    if (mhz <= 1331) {
        pt_switch_release_mem_clock();
        return;
    }
    pt_switch_set_mem_mhz(mhz);
    g_clock_override_set[2] = false;
}

extern "C" bool pt_switch_clocks(unsigned* cpu_hz, unsigned* gpu_hz) {
    if (!EnsureClocks()) return false;
    u32 cpu = 0, gpu = 0;
    if (g_clk_state == 1) {
        if (R_FAILED(clkrstGetClockRate(&g_clk_cpu, &cpu)) || R_FAILED(clkrstGetClockRate(&g_clk_gpu, &gpu))) return false;
    } else {
        if (R_FAILED(pcvGetClockRate(PcvModule_CpuBus, &cpu)) || R_FAILED(pcvGetClockRate(PcvModule_GPU, &gpu))) return false;
    }
    *cpu_hz = cpu;
    *gpu_hz = gpu;
    return true;
}

// the GPU rates the system offers (log only)
extern "C" void pt_switch_log_gpu_rates() {
    if (!EnsureClocks()) {
        pt::LogInfo("clocks: the clock services are not reachable");
        return;
    }
    u32 rates[40] = {};
    s32 count = 0;
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
    const Result rc = g_clk_state == 1 ? clkrstGetPossibleClockRates(&g_clk_gpu, rates, 40, &type, &count)
                                        : pcvGetPossibleClockRates(PcvModule_GPU, rates, 40, &type, &count);
    if (R_FAILED(rc)) {
        pt::LogInfo("clocks: gpu rates not available ({:#x})", static_cast<unsigned>(rc));
        return;
    }
    std::string list;
    for (s32 i = 0; i < count && i < 40; ++i) list += std::to_string(rates[i] / 1000000) + (i + 1 < count ? " " : "");
    pt::LogInfo("clocks: via {}, gpu possible rates ({}, {}): {} MHz", g_clk_state == 1 ? "clkrst" : "pcv",
                type == PcvClockRatesListType_Discrete ? "discrete" : (type == PcvClockRatesListType_Range ? "range" : "?"), count, list);
    unsigned mem[40] = {};
    const unsigned mem_count = pt_switch_mem_rates(mem, 40);
    std::string mem_list;
    for (unsigned i = 0; i < mem_count; ++i) mem_list += std::to_string(mem[i]) + (i + 1 < mem_count ? " " : "");
    pt::LogInfo("clocks: memory {} MHz now, possible rates ({}): {} MHz", pt_switch_mem_hz() / 1000000u, mem_count, mem_count ? mem_list : "none");
    unsigned cpu[40] = {};
    const unsigned cpu_count = pt_switch_cpu_rates(cpu, 40);
    std::string cpu_list;
    for (unsigned i = 0; i < cpu_count; ++i) cpu_list += std::to_string(cpu[i]) + (i + 1 < cpu_count ? " " : "");
    pt::LogInfo("clocks: cpu possible rates ({}): {} MHz", cpu_count, cpu_count ? cpu_list : "none");
}

// Requests a GPU clock (the nearest allowed rate at or below `mhz`) and returns the clock read back afterwards in MHz, 0 on failure.
// Only called when the user asks for it (clocks.ini, nxlink arguments): the system otherwise picks the clock.
// 30 fps lock: the number of VBlanks each queued image stays on screen. nxvk's WSI resets the window to 1 whenever it creates a
// swapchain, so the renderer applies this before every present (it only stores NWindow::swap_interval, which nwindowQueueBuffer sends)
extern "C" void pt_switch_set_swap_interval(unsigned interval) { nwindowSetSwapInterval(nwindowGetDefault(), interval); }

extern "C" unsigned pt_switch_set_gpu_mhz(unsigned mhz) {
    if (!EnsureClocks()) return 0;
    u32 rates[40] = {};
    s32 count = 0;
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
    const Result list = g_clk_state == 1 ? clkrstGetPossibleClockRates(&g_clk_gpu, rates, 40, &type, &count)
                                          : pcvGetPossibleClockRates(PcvModule_GPU, rates, 40, &type, &count);
    if (R_FAILED(list) || count <= 0 || type != PcvClockRatesListType_Discrete) return 0;
    u32 pick = rates[0];
    for (s32 i = 0; i < count && i < 40; ++i) {
        if (rates[i] / 1000000u <= mhz && rates[i] > pick) pick = rates[i];
    }
    // with a clock manager, only through its override: setting the clock directly as well (two writers) is the suspect of a pcv abort
    // (2011-0102) seen on a Horizon OC console; it applies the override on its next tick
    if (ClockManagerOverrideGpu(pick)) return pick / 1000000u;
    const Result set = g_clk_state == 1 ? clkrstSetClockRate(&g_clk_gpu, pick) : pcvSetClockRate(PcvModule_GPU, pick);
    if (R_FAILED(set)) {
        pt::LogInfo("clocks: setting the gpu to {} MHz failed ({:#x})", pick / 1000000, static_cast<unsigned>(set));
        return 0;
    }
    u32 now = 0;
    if (R_FAILED(g_clk_state == 1 ? clkrstGetClockRate(&g_clk_gpu, &now) : pcvGetClockRate(PcvModule_GPU, &now))) return 0;
    return now / 1000000u;
}

// the allowed GPU rates in MHz, ascending; returns how many
extern "C" unsigned pt_switch_gpu_rates(unsigned* out_mhz, unsigned max) {
    if (!EnsureClocks()) return 0;
    u32 rates[40] = {};
    s32 count = 0;
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
    const Result rc = g_clk_state == 1 ? clkrstGetPossibleClockRates(&g_clk_gpu, rates, 40, &type, &count)
                                        : pcvGetPossibleClockRates(PcvModule_GPU, rates, 40, &type, &count);
    if (R_FAILED(rc) || type != PcvClockRatesListType_Discrete) return 0;
    unsigned n = 0;
    for (s32 i = 0; i < count && i < 40 && n < max; ++i) out_mhz[n++] = rates[i] / 1000000u;
    return n;
}

// The CPU clock, for the automatic clock: the allowed rates in MHz, ascending (returns how many); a request for the highest allowed rate at or
// below `mhz`, with a clock manager only through its override (module 0, as for the GPU), without one through clkrst (returns the requested
// rate in MHz, 0 on failure); and the override handed back.
extern "C" unsigned pt_switch_cpu_rates(unsigned* out_mhz, unsigned max) {
    if (!EnsureClocks()) return 0;
    u32 rates[40] = {};
    const s32 count = PossibleRates(&g_clk_cpu, PcvModule_CpuBus, rates, 40);
    unsigned n = 0;
    for (s32 i = 0; i < count && n < max; ++i) out_mhz[n++] = rates[i] / 1000000u;
    return n;
}

extern "C" unsigned pt_switch_set_cpu_mhz(unsigned mhz) {
    if (!EnsureClocks()) return 0;
    u32 rates[40] = {};
    const s32 count = PossibleRates(&g_clk_cpu, PcvModule_CpuBus, rates, 40);
    if (count <= 0) return 0;
    u32 pick = rates[0];
    for (s32 i = 0; i < count; ++i) {
        if (rates[i] / 1000000u <= mhz && rates[i] > pick) pick = rates[i];
    }
    if (ClockManagerOverride(0u, pick)) return pick / 1000000u;
    const Result set = g_clk_state == 1 ? clkrstSetClockRate(&g_clk_cpu, pick) : pcvSetClockRate(PcvModule_CpuBus, pick);
    if (R_FAILED(set)) {
        pt::LogInfo("clocks: setting the cpu to {} MHz failed ({:#x})", pick / 1000000, static_cast<unsigned>(set));
        return 0;
    }
    return pick / 1000000u;
}

extern "C" void pt_switch_release_cpu_clock() {
    if (g_clock_override_set[0]) ClockManagerOverride(0u, 0);
}
