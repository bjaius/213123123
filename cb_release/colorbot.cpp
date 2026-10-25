// language: C++17, file: colorbot.cpp, target: Windows 11, MSVC
// Colorbot (trigger) + Aimbot + Auto Headshot for Valorant external.
// Scan method: GDI BitBlt. Color target: purple HSV 275-320 (Valorant enemy highlight).
// Aimbot: SendInput MOUSEEVENTF_MOVE (relative). Trigger: BroadcastSystemMessage.
// Vanguard note: SendInput mouse movement is detectable via MouClass IRP hook at ring-0.
//   For bypass use: KMDF HID filter driver or hardware HID (Arduino Leonardo).
//   BroadcastSystemMessage trigger is less commonly flagged.

#include "colorbot.hpp"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <random>
#include <vector>
#pragma comment(lib, "user32.lib")

namespace {
    // Purple range: HSV 275-320, S 35-100, V 85-100.
    // Valorant's default enemy highlight color.
    constexpr HSV_RANGE kPurpleRange{ 275.0f, 320.0f, 35.0f, 100.0f, 85.0f, 100.0f };
}

// ---- Lifecycle ---------------------------------------------------------------

ColorBot::~ColorBot() { Stop(); }

void ColorBot::Start() {
    if (running) return;
    running = true;
    worker = std::thread(&ColorBot::Loop, this);
}

void ColorBot::Stop() {
    running = false;
    if (worker.joinable()) worker.join();
}

// ---- Utility -----------------------------------------------------------------

int ColorBot::GetRandomInt(int min, int max) {
    if (min >= max) return min;
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> distr(min, max);
    return distr(gen);
}

void ColorBot::RGBtoHSV(int r, int g, int b, float& h, float& s, float& v) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxc = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
    float minc = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
    float delta = maxc - minc;
    v = maxc * 100.0f;
    if (maxc < 0.00001f) { s = 0.0f; h = 0.0f; return; }
    s = (delta / maxc) * 100.0f;
    if (delta < 0.00001f) { h = 0.0f; return; }
    if      (maxc == rf) h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
    else if (maxc == gf) h = 60.0f * (((bf - rf) / delta) + 2.0f);
    else                 h = 60.0f * (((rf - gf) / delta) + 4.0f);
    if (h < 0.0f) h += 360.0f;
}

bool ColorBot::MatchesHSV(float h, float s, float v, const HSV_RANGE& range) {
    return h >= range.h_low && h <= range.h_high &&
           s >= range.s_low && s <= range.s_high &&
           v >= range.v_low && v <= range.v_high;
}

// ---- GDI capture -------------------------------------------------------------

bool ColorBot::GrabScreen(int x, int y, int w, int h,
                           std::vector<RGBQUAD>& pixels) {
    if (w <= 0 || h <= 0) return false;

    HDC hScreen = GetDC(NULL);
    if (!hScreen) return false;

    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hBmp = hMem ? CreateCompatibleBitmap(hScreen, w, h) : nullptr;
    HBITMAP hOld = hBmp  ? (HBITMAP)SelectObject(hMem, hBmp) : nullptr;

    bool ok = false;
    if (hOld && hOld != HGDI_ERROR) {
        BITMAPINFOHEADER bi = {};
        bi.biSize        = sizeof(bi);
        bi.biWidth       = w;
        bi.biHeight      = -h;  // top-down
        bi.biPlanes      = 1;
        bi.biBitCount    = 32;
        bi.biCompression = BI_RGB;

        pixels.resize(static_cast<size_t>(w) * h);
        bool copied = BitBlt(hMem, 0, 0, w, h, hScreen, x, y, SRCCOPY) != FALSE;
        SelectObject(hMem, hOld);
        ok = copied && GetDIBits(hMem, hBmp, 0, h,
                                  pixels.data(), (BITMAPINFO*)&bi,
                                  DIB_RGB_COLORS) == h;
    }
    if (hBmp)   DeleteObject(hBmp);
    if (hMem)   DeleteDC(hMem);
    if (hScreen) ReleaseDC(NULL, hScreen);
    return ok;
}

// ---- Color scan --------------------------------------------------------------

std::vector<PixelPos> ColorBot::FindPurplePixels(
        const std::vector<RGBQUAD>& pixels, int w, int h,
        const HSV_RANGE& range) {

    std::vector<PixelPos> result;
    result.reserve(512);

    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const RGBQUAD& p = pixels[static_cast<size_t>(row) * w + col];
            float hh, ss, vv;
            RGBtoHSV(p.rgbRed, p.rgbGreen, p.rgbBlue, hh, ss, vv);
            if (MatchesHSV(hh, ss, vv, range))
                result.push_back({ col, row });
        }
    }
    return result;
}

// ---- Head centroid -----------------------------------------------------------
// Sorts purple pixels by Y ascending (top of screen first).
// Takes the top `head_pct`% of them → averages X/Y → that's the head target.
// With headshot_mode=false, takes the full cluster centroid instead (body aim).

bool ColorBot::ComputeHeadCentroid(const std::vector<PixelPos>& pixels,
                                   int head_pct,
                                   float& outCx, float& outCy) {
    if (pixels.empty()) return false;

    // Already sorted or we sort here by Y (ascending = higher on screen).
    std::vector<PixelPos> sorted = pixels;
    std::sort(sorted.begin(), sorted.end(),
              [](const PixelPos& a, const PixelPos& b){ return a.y < b.y; });

    // Take top head_pct% by count, minimum 1 pixel.
    int total = static_cast<int>(sorted.size());
    int take  = total * head_pct / 100;
    if (take < 1) take = 1;

    double sx = 0.0, sy = 0.0;
    for (int i = 0; i < take; ++i) {
        sx += sorted[i].x;
        sy += sorted[i].y;
    }
    outCx = static_cast<float>(sx / take);
    outCy = static_cast<float>(sy / take);
    return true;
}

// ---- Mouse movement ----------------------------------------------------------
// Sends relative mouse move via SendInput.
// Vanguard detects this via MouClass IRP hooks. See header for bypass options.

void ColorBot::MoveMouse(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    INPUT inp = {};
    inp.type      = INPUT_MOUSE;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE;
    inp.mi.dx     = static_cast<LONG>(dx);
    inp.mi.dy     = static_cast<LONG>(dy);
    SendInput(1, &inp, sizeof(INPUT));
}

// ---- Trigger fire ------------------------------------------------------------

static bool BroadcastMessageInject(UINT msg, WPARAM w, LPARAM l) {
    DWORD recipients = BSM_APPLICATIONS;
    LONG r = BroadcastSystemMessageW(
        BSF_IGNORECURRENTTASK | BSF_POSTMESSAGE | BSF_ALLOWSFW,
        &recipients, msg, w, l);
    return r > 0;
}

void ColorBot::FireTrigger(HWND /*targetWindow*/) {
    UINT vkey     = config.shoot_vkey.load();
    UINT scanCode = MapVirtualKeyA(vkey, MAPVK_VK_TO_VSC);
    bool isMouse  = (vkey == VK_LBUTTON || vkey == VK_RBUTTON ||
                     vkey == VK_MBUTTON  || vkey == VK_XBUTTON1 ||
                     vkey == VK_XBUTTON2);

    int holdDelay = GetRandomInt(15, 30);

    POINT curPos; GetCursorPos(&curPos);
    LPARAM mouseLParam = MAKELPARAM(curPos.x, curPos.y);
    LPARAM kdLParam = (LPARAM)(0x00000001 | (scanCode << 16));
    LPARAM kuLParam = (LPARAM)(0xC0000001 | (scanCode << 16));

    UINT mDown = 0, mUp = 0;
    WPARAM mDownW = 0, mUpW = 0;
    if (isMouse) {
        switch (vkey) {
        case VK_LBUTTON:  mDown = WM_LBUTTONDOWN; mUp = WM_LBUTTONUP;
                          mDownW = MK_LBUTTON; break;
        case VK_RBUTTON:  mDown = WM_RBUTTONDOWN; mUp = WM_RBUTTONUP;
                          mDownW = MK_RBUTTON; break;
        case VK_MBUTTON:  mDown = WM_MBUTTONDOWN; mUp = WM_MBUTTONUP;
                          mDownW = MK_MBUTTON; break;
        case VK_XBUTTON1: mDown = WM_XBUTTONDOWN; mUp = WM_XBUTTONUP;
                          mDownW = MAKEWPARAM(MK_XBUTTON1, XBUTTON1);
                          mUpW   = MAKEWPARAM(0, XBUTTON1); break;
        case VK_XBUTTON2: mDown = WM_XBUTTONDOWN; mUp = WM_XBUTTONUP;
                          mDownW = MAKEWPARAM(MK_XBUTTON2, XBUTTON2);
                          mUpW   = MAKEWPARAM(0, XBUTTON2); break;
        default: break;
        }
    }

    if (isMouse) {
        BroadcastMessageInject(mDown, mDownW, mouseLParam);
        std::this_thread::sleep_for(std::chrono::milliseconds(holdDelay));
        BroadcastMessageInject(mUp, mUpW, mouseLParam);
    } else {
        BroadcastMessageInject(WM_KEYDOWN, vkey, kdLParam);
        std::this_thread::sleep_for(std::chrono::milliseconds(holdDelay));
        BroadcastMessageInject(WM_KEYUP, vkey, kuLParam);
    }
}

// ---- Main loop ---------------------------------------------------------------

void ColorBot::Loop() {
    std::vector<RGBQUAD> pixels;
    auto lastFireTime = std::chrono::steady_clock::now();

    while (running) {
        bool aimOn     = config.aimbot_enabled.load();
        bool triggerOn = config.enabled.load();

        if (!aimOn && !triggerOn) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);

        // ---- Determine scan region -------------------------------------------
        // Aimbot uses aim_fov (large area). Trigger-only uses the small capture box.
        RECT scanRect;
        if (aimOn) {
            int fov = config.aim_fov.load();
            scanRect = CaptureArea::AimBounds(screenW, screenH, fov);
        } else {
            scanRect = CaptureArea::Bounds(screenW, screenH,
                                            config.capture_x.load(),
                                            config.capture_y.load());
        }

        int captureX = scanRect.left;
        int captureY = scanRect.top;
        int captureW = scanRect.right  - scanRect.left;
        int captureH = scanRect.bottom - scanRect.top;

        if (!GrabScreen(captureX, captureY, captureW, captureH, pixels)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // ---- Build tolerance-adjusted HSV range ------------------------------
        int tol = config.tolerance.load();
        HSV_RANGE range = kPurpleRange;
        range.s_low  = (std::max)(0.0f,   range.s_low  - static_cast<float>(tol));
        range.s_high = (std::min)(100.0f, range.s_high + static_cast<float>(tol));
        range.v_low  = (std::max)(0.0f,   range.v_low  - static_cast<float>(tol));
        range.v_high = (std::min)(100.0f, range.v_high + static_cast<float>(tol));

        // ---- Find purple pixels ----------------------------------------------
        std::vector<PixelPos> purplePixels =
            FindPurplePixels(pixels, captureW, captureH, range);

        if (purplePixels.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // ---- Compute target centroid -----------------------------------------
        float localCx = 0.f, localCy = 0.f;
        int headPct = config.headshot_mode.load() ? config.head_percent.load() : 100;
        if (!ComputeHeadCentroid(purplePixels, headPct, localCx, localCy)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Convert local capture coords → screen coords → offset from center
        float targetScreenX = captureX + localCx;
        float targetScreenY = captureY + localCy;
        float screenCenterX = screenW / 2.0f;
        float screenCenterY = screenH / 2.0f;
        float dx = targetScreenX - screenCenterX;
        float dy = targetScreenY - screenCenterY;
        float dist = sqrtf(dx * dx + dy * dy);

        // ---- Aimbot: move mouse toward head centroid -------------------------
        if (aimOn) {
            int smooth = (std::max)(1, config.aim_smoothing.load());
            // Compute step: at minimum ±1px so we always converge.
            int mx = static_cast<int>(dx / smooth);
            int my = static_cast<int>(dy / smooth);
            // Sub-pixel fallback: if rounding collapses to 0, nudge by 1.
            if (mx == 0 && fabsf(dx) > 0.3f) mx = (dx > 0.f) ? 1 : -1;
            if (my == 0 && fabsf(dy) > 0.3f) my = (dy > 0.f) ? 1 : -1;
            MoveMouse(mx, my);
        }

        // ---- Trigger: fire when crosshair is on head centroid ----------------
        if (triggerOn) {
            int thresh = config.trigger_threshold.load();
            if (dist <= static_cast<float>(thresh)) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastFireTime).count();

                if (elapsed >= config.cooldown_ms.load()) {
                    int delay = GetRandomInt(config.delay_min_ms.load(),
                                            config.delay_max_ms.load());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));

                    HWND fw = GetForegroundWindow();
                    if (fw) FireTrigger(fw);
                    lastFireTime = std::chrono::steady_clock::now();
                }
            }
        }

        // ~1ms loop; aimbot needs to be responsive, not sleeping 10ms.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
