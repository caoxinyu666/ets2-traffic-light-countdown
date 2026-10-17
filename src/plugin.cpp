// ETS2 信号灯倒计时 HUD
//
// 独立读取游戏内的信号灯对象；遥测接口只提供卡车位置。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "MinHook.h"
#include "scssdk_telemetry.h"
#include "panel_input.hpp"
#include "native_signal.hpp"

namespace {
HMODULE module;
std::wstring iniPath;
scs_log_t logger = nullptr;
std::recursive_mutex renderMutex;

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn originalPresent = nullptr;
ResizeFn originalResize = nullptr;
void* presentTarget = nullptr;
void* resizeTarget = nullptr;
IDXGISwapChain* activeChain = nullptr;
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
ID3D11RenderTargetView* target = nullptr;
HWND gameWindow = nullptr;
WNDPROC oldProc = nullptr;
ImGuiContext* ui = nullptr;
std::atomic<bool> stopping{false};
bool panel = false, settingsHeld = false, f11Held = false;
bool redHeld = false, greenHeld = false, clearHeld = false;
std::atomic<bool> captureMouse{false};
std::atomic<int> wheelDelta{0};
PanelInput panelInput;
bool focusPanel = false;
LARGE_INTEGER frequency{}, lastTick{};

// ---- 遥测 ----
std::mutex clockMutex;
double truckSpeed = 0; // m/s
double truckX = 0, truckZ = 0, truckHeading = 0;
bool truckPositionValid = false;
bool driving = false;
NativeSignalReader signalReader;
bool signalReaderReady = false;
std::array<NativeSignal, 256> nativeSignals{};
unsigned nativeSignalCount = 0;
ULONGLONG lastSignalPoll = 0;

void SCSAPIFUNC onChannel(const char*, scs_u32_t, const scs_value_t* v, void* ctx) {
    if (!v) return;
    const int which = static_cast<int>(reinterpret_cast<uintptr_t>(ctx));
    if (which == 1) {
        if (v->type != SCS_VALUE_TYPE_dplacement) return;
        std::lock_guard<std::mutex> lock(clockMutex);
        truckX = v->value_dplacement.position.x;
        truckZ = v->value_dplacement.position.z;
        truckHeading = v->value_dplacement.orientation.heading;
        truckPositionValid = true;
        return;
    }
    double n = 0;
    switch (v->type) {
    case SCS_VALUE_TYPE_u32: n = v->value_u32.value; break;
    case SCS_VALUE_TYPE_s32: n = v->value_s32.value; break;
    case SCS_VALUE_TYPE_float: n = v->value_float.value; break;
    default: return;
    }
    if (!std::isfinite(n)) return;
    std::lock_guard<std::mutex> lock(clockMutex);
    if (which == 0) truckSpeed = std::max(0.0, n);
}

void SCSAPIFUNC onEvent(scs_event_t event, const void*, void*) {
    if (event == SCS_TELEMETRY_EVENT_frame_end && signalReaderReady) {
        if (!driving || GetTickCount64() - lastSignalPoll < 100) return;
        lastSignalPoll = GetTickCount64();
        std::array<NativeSignal, 256> fresh{};
        const unsigned count = signalReader.poll(fresh);
        std::lock_guard<std::mutex> lock(clockMutex);
        nativeSignals = fresh;
        nativeSignalCount = count;
        return;
    }
    std::lock_guard<std::mutex> lock(clockMutex);
    if (event == SCS_TELEMETRY_EVENT_started) driving = true;
    if (event == SCS_TELEMETRY_EVENT_paused) { driving = false; nativeSignalCount = 0; }
}

void log(const char* s) { if (logger) logger(SCS_LOG_TYPE_message, s); }

// ---- 设置 ----
struct Settings {
    bool enabled = true;
    double green = 30, yellow = 4, red = 25, yellow2 = 4;
    float x = 0.5f, y = 0.07f, font = 24, opacity = 1;
    float detectionDistance = 250;
    int key = VK_BACK, hideKey = VK_F11;
    bool keyCtrl = true, keyShift = true;
    double cycle() const { return green + yellow + red + yellow2; }
} settings;

// 驾驶期间累计的真实秒数。没有同步点时绝不显示推算的灯色。
double realClock = 0;
enum Light { LightGreen, LightYellow, LightRed, LightYellow2 };
bool synchronizedLight = false;
double synchronizedAt = 0;
double synchronizedPhase = 0;
double distanceSinceSync = 0;
constexpr double kClearDistance = 120.0;

struct LiveLight {
    Light light;
    double remaining;
    double distance;
    bool valid = false;
};
LiveLight currentLive;

LiveLight findLiveLight(double x, double z, double heading) {
    std::array<NativeSignal, 256> signals;
    unsigned count = 0;
    { std::lock_guard<std::mutex> lock(clockMutex);
        signals = nativeSignals;
        count = nativeSignalCount;
    }
    const double angle = heading * 6.283185307179586;
    const double forwardX = -std::sin(angle), forwardZ = -std::cos(angle);
    LiveLight best{};
    double bestScore = 1e9;
    for (unsigned i = 0; i < count; ++i) {
        const auto& r = signals[i];
        const double dx = r.x - x, dz = r.z - z;
        const double ahead = dx * forwardX + dz * forwardZ;
        const double lateral = std::abs(dx * forwardZ - dz * forwardX);
        const double lateralLimit = std::min(30.0, std::max(18.0, ahead * 0.12));
        if (ahead < 2 || ahead > settings.detectionDistance || lateral > lateralLimit) continue;
        const double score = ahead + lateral * 2;
        if (score < bestScore) {
            bestScore = score;
            best = {r.state == 8 ? LightGreen : (r.state == 2 ? LightRed : LightYellow),
                    r.remaining, std::hypot(dx, dz), true};
        }
    }
    return best;
}
int readInt(const wchar_t* section, const wchar_t* name, int def) {
    return GetPrivateProfileIntW(section, name, def, iniPath.c_str());
}
float readFloat(const wchar_t* name, float def) {
    wchar_t buffer[64];
    GetPrivateProfileStringW(L"Display", name, L"", buffer, 64, iniPath.c_str());
    if (!*buffer) return def;
    wchar_t* end = nullptr;
    const float value = wcstof(buffer, &end);
    return end != buffer && std::isfinite(value) ? value : def;
}
double readCycle(const wchar_t* name, double def) {
    wchar_t buffer[64];
    GetPrivateProfileStringW(L"Cycle", name, L"", buffer, 64, iniPath.c_str());
    if (!*buffer) return def;
    wchar_t* end = nullptr;
    const double value = wcstod(buffer, &end);
    return end != buffer && std::isfinite(value) ? value : def;
}

void loadSettings() {
    settings.enabled = readInt(L"Display", L"Enabled", 1) != 0;
    settings.x = std::clamp(readFloat(L"X", 0.5f), 0.05f, 0.95f);
    settings.y = std::clamp(readFloat(L"Y", 0.07f), 0.f, 0.9f);
    settings.font = std::clamp(readFloat(L"FontSize", 24), 12.f, 48.f);
    settings.opacity = std::clamp(readFloat(L"Opacity", 1), 0.2f, 1.f);
    settings.detectionDistance = std::clamp(readFloat(L"DetectionDistance", 250), 100.f, 400.f);
    settings.key = std::clamp(readInt(L"Hotkeys", L"Settings", VK_BACK), 1, 254);
    settings.keyCtrl = readInt(L"Hotkeys", L"SettingsCtrl", 1) != 0;
    settings.keyShift = readInt(L"Hotkeys", L"SettingsShift", 1) != 0;
    settings.hideKey = std::clamp(readInt(L"Hotkeys", L"Toggle", VK_F11), 1, 254);
    settings.green = std::max(1.0, readCycle(L"Green", 30));
    settings.yellow = std::max(0.0, readCycle(L"Yellow", 4));
    settings.red = std::max(1.0, readCycle(L"Red", 25));
    settings.yellow2 = std::max(0.0, readCycle(L"Yellow2", 4));
}

void saveSettings() {
    auto number = [&](const wchar_t* section, const wchar_t* key, double v) {
        wchar_t buffer[64];
        swprintf(buffer, 64, L"%.2f", v);
        WritePrivateProfileStringW(section, key, buffer, iniPath.c_str());
    };
    auto flag = [&](const wchar_t* section, const wchar_t* key, bool on) {
        WritePrivateProfileStringW(section, key, on ? L"1" : L"0", iniPath.c_str());
    };
    flag(L"Display", L"Enabled", settings.enabled);
    number(L"Display", L"X", settings.x);
    number(L"Display", L"Y", settings.y);
    number(L"Display", L"FontSize", settings.font);
    number(L"Display", L"Opacity", settings.opacity);
    number(L"Display", L"DetectionDistance", settings.detectionDistance);
    number(L"Cycle", L"Green", settings.green);
    number(L"Cycle", L"Yellow", settings.yellow);
    number(L"Cycle", L"Red", settings.red);
    number(L"Cycle", L"Yellow2", settings.yellow2);
}

// ---- 相位推算 ----
struct Phase {
    Light light = LightGreen;
    double remaining = 0;   // 当前相位还剩多少秒（游戏时间）
    double elapsed = 0;     // 当前相位已过去多少秒
};

// 把「周期内某一时刻」映射成灯色与剩余时间。
Phase phaseAt(double secondsIntoCycle) {
    const double cycle = settings.cycle();
    double t = fmod(secondsIntoCycle, cycle);
    if (t < 0) t += cycle;
    Phase result;
    if (t < settings.green) {
        result.light = LightGreen;
        result.remaining = settings.green - t;
        result.elapsed = t;
    } else if (t < settings.green + settings.yellow) {
        result.light = LightYellow;
        result.remaining = settings.green + settings.yellow - t;
        result.elapsed = t - settings.green;
    } else if (t < settings.green + settings.yellow + settings.red) {
        result.light = LightRed;
        result.remaining = settings.green + settings.yellow + settings.red - t;
        result.elapsed = t - settings.green - settings.yellow;
    } else {
        result.light = LightYellow2;
        result.remaining = cycle - t;
        result.elapsed = t - settings.green - settings.yellow - settings.red;
    }
    return result;
}

const char* lightName(Light light) {
    switch (light) {
    case LightGreen: return "绿灯";
    case LightYellow: return "黄灯";
    case LightRed: return "红灯";
    default: return "黄灯";
    }
}
ImU32 lightColor(Light light, int alpha) {
    switch (light) {
    case LightGreen: return IM_COL32(60, 220, 90, alpha);
    case LightYellow: return IM_COL32(250, 200, 40, alpha);
    case LightRed: return IM_COL32(240, 60, 55, alpha);
    default: return IM_COL32(250, 200, 40, alpha);
    }
}

// ---- DX11 / ImGui 生命周期（沿用货物信息插件的成熟实现）----
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

LRESULT CALLBACK gameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (captureMouse) {
        if (m == WM_INPUT) {
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != UINT(-1) && raw.header.dwType == RIM_TYPEMOUSE) {
                panelInput.raw(raw.data.mouse);
                return DefWindowProcW(h, m, w, l);
            }
        }
        if (m == WM_MOUSEWHEEL) { wheelDelta += GET_WHEEL_DELTA_WPARAM(w); return 0; }
        if (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_RBUTTONDOWN || m == WM_RBUTTONUP ||
            m == WM_MOUSEMOVE || m == WM_MOUSEWHEEL) return 0;
        if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) {
            if (w == VK_TAB || w == VK_SPACE || w == VK_RETURN || w == VK_LEFT || w == VK_RIGHT ||
                w == VK_UP || w == VK_DOWN || w == VK_PRIOR || w == VK_NEXT || w == VK_HOME ||
                w == VK_END || w == VK_ESCAPE) return 0;
        }
    }
    return CallWindowProcW(oldProc, h, m, w, l);
}

void resetRender() {
    captureMouse = false;
    panelInput.stop();
    if (oldProc && gameWindow && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(gameWindow, GWLP_WNDPROC)) == gameProc)
        SetWindowLongPtrW(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oldProc));
    oldProc = nullptr;
    gameWindow = nullptr;
    if (ui) {
        ImGui::SetCurrentContext(ui);
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(ui);
        ui = nullptr;
    }
    release(target); release(context); release(device); release(activeChain);
}

bool initRender(IDXGISwapChain* chain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow) return false;
    if (FAILED(chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) return false;
    device->GetImmediateContext(&context);
    activeChain = chain; chain->AddRef();
    gameWindow = desc.OutputWindow;

    ui = ImGui::CreateContext();
    ImGui::SetCurrentContext(ui);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAlways = true;

    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring fontPath = std::wstring(windows) + L"\\Fonts\\msyh.ttc";
    char path[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, fontPath.c_str(), -1, path, MAX_PATH, nullptr, nullptr);
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2; fontConfig.OversampleV = 2;
    if (!io.Fonts->AddFontFromFileTTF(path, 26, &fontConfig, io.Fonts->GetGlyphRangesChineseFull()))
        io.Fonts->AddFontDefault();

    if (!ImGui_ImplDX11_Init(device, context)) { resetRender(); return false; }
    ImGui::StyleColorsDark();
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&lastTick);

    oldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(gameWindow, GWLP_WNDPROC,
                                                          reinterpret_cast<LONG_PTR>(gameProc)));
    log("[TrafficTimer] Direct3D 11 in-game HUD ready. Ctrl+Shift+Backspace=settings, F11=toggle.");
    return true;
}

std::string seconds(double value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.0f", std::max(0.0, value));
    return buffer;
}

void drawHUD() {
    auto& io = ImGui::GetIO();
    auto* draw = ImGui::GetBackgroundDrawList();
    const float scale = io.DisplaySize.y / 972.f;
    const float fontSize = settings.font * scale;

    Phase current = phaseAt(synchronizedPhase + realClock - synchronizedAt);
    if (currentLive.valid) {
        current.light = currentLive.light;
        current.remaining = currentLive.remaining;
    }

    // 文本
    char mainText[64];
    snprintf(mainText, sizeof(mainText), "%s  还剩 %s 秒", lightName(current.light),
             seconds(std::ceil(current.remaining)).c_str());
    const char* subText = currentLive.valid ? "游戏实时信号灯" : "手动同步 · F7 清除";

    auto* font = ImGui::GetFont();
    const float lineHeight = fontSize * 1.45f;
    const float dot = fontSize * 0.42f;
    const float pad = fontSize * 0.7f;

    const float width1 = font->CalcTextSizeA(fontSize, FLT_MAX, 0, mainText).x;
    const float width3 = font->CalcTextSizeA(fontSize * 0.62f, FLT_MAX, 0, subText).x;
    const float textWidth = std::max(width1, width3);
    const float boxWidth = textWidth + pad * 2 + dot * 2 + fontSize;
    const float boxHeight = lineHeight + fontSize * 1.15f + pad * 2;

    const float centerX = settings.x * io.DisplaySize.x;
    const float top = settings.y * io.DisplaySize.y;
    const float left = centerX - boxWidth / 2;

    const int alpha = static_cast<int>(255 * settings.opacity);
    draw->AddRectFilled({left, top}, {left + boxWidth, top + boxHeight},
                        IM_COL32(12, 14, 18, static_cast<int>(190 * settings.opacity)), fontSize * 0.3f);

    const float cx = left + pad + dot;
    const float cy = top + pad + fontSize * 0.62f;
    draw->AddCircleFilled({cx, cy}, dot, lightColor(current.light, alpha));
    draw->AddCircle({cx, cy}, dot, IM_COL32(255, 255, 255, static_cast<int>(120 * settings.opacity)),
                    20, std::max(1.f, fontSize / 14));
    draw->AddText(font, fontSize, {left + pad + dot * 2 + fontSize * 0.4f, top + pad},
                  IM_COL32(255, 255, 255, alpha), mainText);
    draw->AddText(font, fontSize * 0.62f,
                  {left + pad, top + pad + lineHeight + fontSize * 0.15f},
                  IM_COL32(180, 190, 200, alpha), subText);
}

void drawSettings() {
    const auto screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({35, 35}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({std::min(560.f, screen.x - 70), std::min(760.f, screen.y - 70)}, ImGuiCond_FirstUseEver);
    bool changed = false;
    ImGui::Begin("信号灯倒计时设置", &panel);
    ImGui::SetWindowFontScale(0.8f);
    if (focusPanel) { ImGui::SetWindowFocus(); focusPanel = false; }
    ImGui::TextUnformatted("F8：红灯刚亮  F9：绿灯刚亮  F7：清除");
    ImGui::TextUnformatted("Ctrl+Shift+Backspace：关闭面板  F11：显示 / 隐藏");
    ImGui::Text("游戏信号灯读取：%s", signalReaderReady ? "已启用" : "此版本不支持");
    ImGui::TextWrapped("检测到前方信号灯时自动显示真实倒计时。"
                       "没有该数据时，可在看到灯刚变色时按 F8/F9 手动同步；驶离约 120 米后清除。");

    changed |= ImGui::Checkbox("启用倒计时", &settings.enabled);
    changed |= ImGui::SliderFloat("识别距离（米）", &settings.detectionDistance, 100, 400, "%.0f 米");
    ImGui::TextWrapped("距离设得越远，复杂路口越可能选到邻近道路的灯。");
    ImGui::Separator();
    ImGui::TextUnformatted("周期设置（秒）—— 必须与配套 .scs 一致");
    auto duration = [&](const char* name, double& value, float min, float max) {
        float temp = static_cast<float>(value);
        if (ImGui::SliderFloat(name, &temp, min, max, "%.0f")) { value = temp; changed = true; }
    };
    duration("绿灯", settings.green, 5, 90);
    duration("黄灯", settings.yellow, 0, 10);
    duration("红灯", settings.red, 5, 90);
    duration("黄灯（红后）", settings.yellow2, 0, 10);
    ImGui::Text("当前周期：%.0f 秒", settings.cycle());
    ImGui::Separator();
    changed |= ImGui::SliderFloat("水平位置", &settings.x, 0.05f, 0.95f, "%.3f");
    changed |= ImGui::SliderFloat("垂直位置", &settings.y, 0.f, 0.9f, "%.3f");
    changed |= ImGui::SliderFloat("字号", &settings.font, 12, 48, "%.0f");
    changed |= ImGui::SliderFloat("不透明度", &settings.opacity, 0.2f, 1.f, "%.2f");
    if (ImGui::Button("恢复默认位置")) {
        settings.x = 0.5f; settings.y = 0.07f; settings.font = 24; settings.opacity = 1; changed = true;
    }
    ImGui::End();
    if (changed) saveSettings();
}

HRESULT WINAPI hookedPresent(IDXGISwapChain* chain, UINT sync, UINT flags) {
    {
        std::lock_guard<std::recursive_mutex> guard(renderMutex);
        if (!stopping && !(flags & DXGI_PRESENT_TEST)) {
            ImGuiContext* previous = ImGui::GetCurrentContext();
            if (activeChain && activeChain != chain) {
                DXGI_SWAP_CHAIN_DESC next{};
                if (SUCCEEDED(chain->GetDesc(&next)) && next.OutputWindow == gameWindow) resetRender();
            }
            if (!activeChain) initRender(chain);
            if (ui && activeChain == chain) {
                ImGui::SetCurrentContext(ui);
                const bool foreground = GetForegroundWindow() == gameWindow;
                bool pressed = foreground && (GetAsyncKeyState(settings.key) & 0x8000) &&
                    (!settings.keyCtrl || (GetAsyncKeyState(VK_CONTROL) & 0x8000)) &&
                    (!settings.keyShift || (GetAsyncKeyState(VK_SHIFT) & 0x8000));
                if (pressed && !settingsHeld) panel = !panel;
                settingsHeld = pressed;
                pressed = foreground && (GetAsyncKeyState(settings.hideKey) & 0x8000);
                if (pressed && !f11Held) { settings.enabled = !settings.enabled; saveSettings(); }
                f11Held = pressed;
                pressed = foreground && !panel && (GetAsyncKeyState(VK_F8) & 0x8000);
                if (pressed && !redHeld) {
                    synchronizedLight = true;
                    synchronizedAt = realClock;
                    synchronizedPhase = settings.green + settings.yellow;
                    distanceSinceSync = 0;
                }
                redHeld = pressed;
                pressed = foreground && !panel && (GetAsyncKeyState(VK_F9) & 0x8000);
                if (pressed && !greenHeld) {
                    synchronizedLight = true;
                    synchronizedAt = realClock;
                    synchronizedPhase = 0;
                    distanceSinceSync = 0;
                }
                greenHeld = pressed;
                pressed = foreground && !panel && (GetAsyncKeyState(VK_F7) & 0x8000);
                if (pressed && !clearHeld) synchronizedLight = false;
                clearHeld = pressed;
                if (panel && foreground && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) panel = false;

                ID3D11Texture2D* back = nullptr;
                if (!target && SUCCEEDED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                          reinterpret_cast<void**>(&back)))) {
                    device->CreateRenderTargetView(back, nullptr, &target);
                    back->Release();
                }
                if (target) {
                    DXGI_SWAP_CHAIN_DESC desc{};
                    chain->GetDesc(&desc);
                    ID3D11Resource* resource = nullptr;
                    target->GetResource(&resource);
                    ID3D11Texture2D* texture = nullptr;
                    resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
                    D3D11_TEXTURE2D_DESC textureDesc{};
                    texture->GetDesc(&textureDesc);
                    texture->Release(); resource->Release();

                    auto& io = ImGui::GetIO();
                    io.DisplaySize = {static_cast<float>(textureDesc.Width), static_cast<float>(textureDesc.Height)};
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    const double elapsed = std::max(0.0,
                        double(now.QuadPart - lastTick.QuadPart) / frequency.QuadPart);
                    io.DeltaTime = std::clamp(static_cast<float>(elapsed), .001f, .1f);
                    lastTick = now;

                    const bool inputActive = panel && foreground;
                    if (inputActive && !captureMouse) {
                        captureMouse = true; focusPanel = true;
                        if (!panelInput.start(gameWindow, io.DisplaySize.x, io.DisplaySize.y))
                            log("[TrafficTimer] Raw mouse registration failed; keyboard navigation remains available.");
                    }
                    if (!inputActive && captureMouse) { captureMouse = false; panelInput.stop(); }
                    panelInput.updateBounds(io.DisplaySize.x, io.DisplaySize.y);
                    const auto mouse = panelInput.snapshot();
                    io.AddMousePosEvent(inputActive ? mouse.x : -FLT_MAX, inputActive ? mouse.y : -FLT_MAX);
                    io.AddMouseButtonEvent(0, inputActive && mouse.left);
                    io.AddMouseButtonEvent(1, inputActive && mouse.right);
                    io.MouseDrawCursor = inputActive;
                    const int wheel = wheelDelta.exchange(0) + mouse.wheel;
                    if (inputActive && wheel) io.AddMouseWheelEvent(0, static_cast<float>(wheel) / WHEEL_DELTA);

                    struct NavKey { int vk; ImGuiKey key; };
                    const NavKey navKeys[] = {
                        {VK_TAB, ImGuiKey_Tab}, {VK_SPACE, ImGuiKey_Space}, {VK_RETURN, ImGuiKey_Enter},
                        {VK_LEFT, ImGuiKey_LeftArrow}, {VK_RIGHT, ImGuiKey_RightArrow},
                        {VK_UP, ImGuiKey_UpArrow}, {VK_DOWN, ImGuiKey_DownArrow},
                        {VK_PRIOR, ImGuiKey_PageUp}, {VK_NEXT, ImGuiKey_PageDown},
                        {VK_HOME, ImGuiKey_Home}, {VK_END, ImGuiKey_End}};
                    for (const auto& key : navKeys)
                        io.AddKeyEvent(key.key, inputActive && (GetAsyncKeyState(key.vk) & 0x8000));
                    io.AddKeyEvent(ImGuiMod_Shift, inputActive && (GetAsyncKeyState(VK_SHIFT) & 0x8000));

                    ImGui_ImplDX11_NewFrame();
                    ImGui::NewFrame();
                    bool isDriving = false;
                    double speed = 0, x = 0, z = 0, heading = 0;
                    bool positionValid = false;
                    { std::lock_guard<std::mutex> lock(clockMutex);
                        isDriving = driving; speed = truckSpeed;
                        x = truckX; z = truckZ; heading = truckHeading;
                        positionValid = truckPositionValid;
                    }
                    if (isDriving) {
                        realClock += elapsed;
                        if (synchronizedLight) {
                            distanceSinceSync += speed * elapsed;
                            if (distanceSinceSync >= kClearDistance) synchronizedLight = false;
                        }
                    }
                    currentLive = isDriving && positionValid ? findLiveLight(x, z, heading) : LiveLight{};
                    if (currentLive.valid) synchronizedLight = false;
                    if (settings.enabled && isDriving && (currentLive.valid || synchronizedLight)) drawHUD();
                    if (panel) drawSettings();
                    if (!panel && captureMouse) { captureMouse = false; panelInput.stop(); }
                    ImGui::Render();

                    ID3D11RenderTargetView* oldTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
                    ID3D11DepthStencilView* depth = nullptr;
                    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldTargets, &depth);
                    context->OMSetRenderTargets(1, &target, nullptr);
                    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldTargets, depth);
                    for (auto& old : oldTargets) release(old);
                    release(depth);
                }
            }
            ImGui::SetCurrentContext(previous);
        }
    }
    const HRESULT result = originalPresent(chain, sync, flags);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        std::lock_guard<std::recursive_mutex> lock(renderMutex);
        resetRender();
    }
    return result;
}

HRESULT WINAPI hookedResize(IDXGISwapChain* chain, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    std::lock_guard<std::recursive_mutex> guard(renderMutex);
    if (chain == activeChain) release(target);
    return originalResize(chain, count, w, h, fmt, flags);
}

bool installHooks() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = module;
    wc.lpszClassName = L"TrafficTimerD3DProbe";
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 32, 32, nullptr, nullptr, module, nullptr);
    if (!window) return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 32;
    desc.BufferDesc.Height = 32;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* chain = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &desc, &chain, &dev, nullptr, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &desc, &chain, &dev, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr)) {
        auto table = *reinterpret_cast<void***>(chain);
        presentTarget = table[8];
        resizeTarget = table[13];
        if (MH_Initialize() == MH_OK &&
            MH_CreateHook(presentTarget, reinterpret_cast<void*>(hookedPresent),
                          reinterpret_cast<void**>(&originalPresent)) == MH_OK &&
            MH_CreateHook(resizeTarget, reinterpret_cast<void*>(hookedResize),
                          reinterpret_cast<void**>(&originalResize)) == MH_OK &&
            MH_EnableHook(presentTarget) == MH_OK && MH_EnableHook(resizeTarget) == MH_OK) ok = true;
    }
    release(ctx); release(dev); release(chain);
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, module);
    if (!ok) {
        if (presentTarget) { MH_DisableHook(presentTarget); MH_RemoveHook(presentTarget); }
        if (resizeTarget) { MH_DisableHook(resizeTarget); MH_RemoveHook(resizeTarget); }
        MH_Uninitialize();
    }
    return ok;
}
}  // namespace

extern "C" SCSAPI_RESULT scs_telemetry_init(scs_u32_t version, const scs_telemetry_init_params_t* params) {
    if (SCS_GET_MAJOR_VERSION(version) != 1 || !params) return SCS_RESULT_unsupported;
    const auto* p = static_cast<const scs_telemetry_init_params_v100_t*>(params);
    logger = p->common.log;

    wchar_t file[32768];
    GetModuleFileNameW(module, file, 32768);
    iniPath = file;
    iniPath = iniPath.substr(0, iniPath.find_last_of(L'.')) + L".ini";
    loadSettings();
    stopping = false;
    synchronizedLight = false;
    currentLive = {};
    realClock = 0;
    signalReaderReady = signalReader.initialize();
    log(signalReaderReady ? "[TrafficTimer] Native signal reader ready."
                          : "[TrafficTimer] Game signal patterns not found; automatic countdown disabled.");
    nativeSignalCount = 0;
    lastSignalPoll = 0;
    { std::lock_guard<std::mutex> lock(clockMutex);
        truckPositionValid = false; truckSpeed = 0; driving = false;
    }

    if (!installHooks()) {
        log("[TrafficTimer] D3D11 hooks failed; plugin was not activated.");
        return SCS_RESULT_generic_error;
    }
    p->register_for_channel("truck.speed", SCS_U32_NIL, SCS_VALUE_TYPE_float,
                            SCS_TELEMETRY_CHANNEL_FLAG_no_value, onChannel, reinterpret_cast<void*>(0));
    p->register_for_channel("truck.world.placement", SCS_U32_NIL, SCS_VALUE_TYPE_dplacement,
                            SCS_TELEMETRY_CHANNEL_FLAG_no_value, onChannel, reinterpret_cast<void*>(1));
    for (auto event : {SCS_TELEMETRY_EVENT_started, SCS_TELEMETRY_EVENT_paused,
                       SCS_TELEMETRY_EVENT_frame_end})
        p->register_for_event(event, onEvent, nullptr);

    log("[TrafficTimer] Loaded. F8=red transition, F9=green transition, F7=clear.");
    return SCS_RESULT_ok;
}

extern "C" SCSAPI_VOID scs_telemetry_shutdown() {
    stopping = true;
    captureMouse = false;
    if (presentTarget) MH_DisableHook(presentTarget);
    if (resizeTarget) MH_DisableHook(resizeTarget);
    { std::lock_guard<std::recursive_mutex> lock(renderMutex); resetRender(); }
    if (presentTarget) MH_RemoveHook(presentTarget);
    if (resizeTarget) MH_RemoveHook(resizeTarget);
    MH_Uninitialize();
    log("[TrafficTimer] Shut down.");
    logger = nullptr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
