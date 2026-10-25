#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <vector>
#include <utility>

namespace CaptureArea {
    inline constexpr int kMinSize = 1;
    inline constexpr int kMaxSize = 20;
    inline constexpr int kDefaultSize = 9;

    inline int ClampSize(int size) {
        return size < kMinSize ? kMinSize : (size > kMaxSize ? kMaxSize : size);
    }

    inline RECT Bounds(int screenWidth, int screenHeight, int width, int height) {
        width  = ClampSize(width);
        height = ClampSize(height);
        int left = (screenWidth  - width)  / 2;
        int top  = (screenHeight - height) / 2;
        return RECT{ left, top, left + width, top + height };
    }

    // Aimbot scan area: larger FOV-based square around screen center.
    inline RECT AimBounds(int screenWidth, int screenHeight, int halfSide) {
        if (halfSide < 1) halfSide = 1;
        int left = screenWidth  / 2 - halfSide;
        int top  = screenHeight / 2 - halfSide;
        int right  = left + halfSide * 2;
        int bottom = top  + halfSide * 2;
        if (left   < 0) left   = 0;
        if (top    < 0) top    = 0;
        if (right  > screenWidth)  right  = screenWidth;
        if (bottom > screenHeight) bottom = screenHeight;
        return RECT{ left, top, right, bottom };
    }
}

struct HSV_RANGE {
    float h_low, h_high;
    float s_low, s_high;
    float v_low, v_high;
};

struct BotConfig {
    // ---- Trigger (original colorbot) ----
    std::atomic<bool> enabled{ false };
    std::atomic<int>  tolerance{ 25 };
    std::atomic<int>  cooldown_ms{ 400 };
    std::atomic<int>  delay_min_ms{ 30 };
    std::atomic<int>  delay_max_ms{ 70 };
    std::atomic<int>  shoot_vkey{ 0x4F };       // default 'O'
    std::atomic<int>  toggle_vkey{ 0x05 };      // default MB4
    std::atomic<int>  capture_x{ CaptureArea::kDefaultSize };
    std::atomic<int>  capture_y{ CaptureArea::kDefaultSize };
    std::atomic<bool> show_capture_area{ false };

    // ---- Aimbot ----
    std::atomic<bool> aimbot_enabled{ false };
    std::atomic<int>  aim_fov{ 150 };           // scan half-size in px (150 -> 300x300 area)
    std::atomic<int>  aim_smoothing{ 10 };      // movement divisor; higher = smoother/slower
    std::atomic<bool> headshot_mode{ true };    // true: target top 25% of cluster; false: center mass
    std::atomic<int>  head_percent{ 25 };       // top N% of purple cluster Y = head zone (1-50)
    std::atomic<int>  trigger_threshold{ 8 };   // auto-fire when crosshair is within N px of head centroid
};

// Pixel position (relative coords inside capture buffer).
struct PixelPos { int x, y; };

struct AimTarget {
    bool found = false;
    float dx = 0.f;   // horizontal offset from screen center (positive = right)
    float dy = 0.f;   // vertical offset from screen center  (positive = down)
    float dist = 0.f; // euclidean distance from center
};

class ColorBot {
public:
    ColorBot()  = default;
    ~ColorBot() { Stop(); }

    void Start();
    void Stop();

    BotConfig& GetConfig() { return config; }

private:
    void Loop();

    // Color helpers
    static void  RGBtoHSV(int r, int g, int b, float& h, float& s, float& v);
    static bool  MatchesHSV(float h, float s, float v, const HSV_RANGE& range);
    static int   GetRandomInt(int min, int max);

    // GDI screen capture into a pre-allocated pixel buffer.
    // Returns true on success; fills pixels (row-major, top-down).
    static bool GrabScreen(int x, int y, int w, int h,
                           std::vector<RGBQUAD>& pixels);

    // Find purple pixel positions in the grab buffer.
    static std::vector<PixelPos> FindPurplePixels(
        const std::vector<RGBQUAD>& pixels, int w, int h,
        const HSV_RANGE& range);

    // Compute head centroid from pixel list using the top head_pct% by Y.
    // Returns false if list is empty.
    static bool ComputeHeadCentroid(const std::vector<PixelPos>& pixels,
                                    int head_pct,
                                    float& outCx, float& outCy);

    // Move mouse by (dx, dy) relative using SendInput.
    // NOTE: Vanguard monitors mouse IRPs via MouClass kernel hook.
    //       SendInput is detectable. For hardware-undetectable movement
    //       use a KMDF HID filter driver or an Arduino Leonardo HID device.
    static void MoveMouse(int dx, int dy);

    // Fire the configured shoot key via BroadcastSystemMessage.
    void FireTrigger(HWND targetWindow);

    BotConfig         config;
    std::atomic<bool> running{ false };
    std::thread       worker;
};
