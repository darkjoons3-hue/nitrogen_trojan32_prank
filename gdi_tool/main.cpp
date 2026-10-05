#pragma execution_character_set("utf-8")
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cstring>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:wWinMainCRTStartup")

// ============================================================
// ЭФФЕКТЫ
// ============================================================
enum Effect {
    FX_SCANLINES = 0,
    FX_NOISE,
    FX_GLITCH,
    FX_INVERT,
    FX_GRAYSCALE,
    FX_KALEIDO,
    FX_ROTATE,
    FX_TEAR,
    FX_ZOOM,
    FX_MATRIX,
    FX_COUNT
};

const wchar_t* g_fxNames[FX_COUNT] = {
    L"Scanlines",
    L"TV Noise",
    L"Glitch",
    L"Invert",
    L"Grayscale",
    L"Kaleidoscope",
    L"Screen Rotate",
    L"Screen Tear",
    L"Zoom Blur",
    L"Matrix Rain"
};

// ============================================================
// ГЛОБАЛЫ
// ============================================================
std::atomic<bool> g_fxOn[FX_COUNT];
std::atomic<bool> g_running{false};
std::atomic<bool> g_renderThreadRun{false};
std::atomic<int>  g_intensity{50};
std::atomic<int>  g_targetFps{30};
std::atomic<int>  g_measuredFps{0};

HINSTANCE g_hInst = nullptr;
HWND g_panel      = nullptr;
HWND g_overlay    = nullptr;
HWND g_checkboxes[FX_COUNT] = {};
HWND g_intensitySlider = nullptr;
HWND g_fpsSlider       = nullptr;
HWND g_intensityLabel  = nullptr;
HWND g_fpsLabel        = nullptr;
HWND g_startBtn        = nullptr;
HWND g_fpsDisplay      = nullptr;

const wchar_t* PANEL_CLASS   = L"GdiPanelCls";
const wchar_t* OVERLAY_CLASS = L"GdiOverlayCls";

const int ID_START         = 100;
const int ID_INTENSITY     = 102;
const int ID_FPS_SLIDER    = 103;
const int ID_CHECKBOX_BASE = 200;
const int HOTKEY_TOGGLE    = 1;

int g_sw = 0, g_sh = 0;
double g_kaleidoAngle = 0.0;
double g_rotateAngle  = 0.0;
std::vector<POINT> g_matrixDrops;

std::thread g_renderThread;

// ============================================================
// UTILS
// ============================================================
void PumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// ============================================================
// ЭФФЕКТЫ (работают над DC в маленьком буфере)
// ============================================================
void ApplyScanlines(HDC dc, int w, int h, int intensity) {
    int gap = 2 + (100 - intensity) / 30;
    HBRUSH black = CreateSolidBrush(RGB(0,0,0));
    for (int y = 0; y < h; y += gap * 2) {
        RECT r = { 0, y, w, y + gap };
        FillRect(dc, &r, black);
    }
    DeleteObject(black);
}

void ApplyNoise(HDC dc, int w, int h, int intensity) {
    int count = 20 + intensity * 6;
    for (int i = 0; i < count; ++i) {
        int x = rand() % (w - 4);
        int y = rand() % (h - 4);
        int sz = 2 + rand() % (2 + intensity / 10);
        int v = rand() & 0xFF;
        HBRUSH b = CreateSolidBrush(RGB(v, v, v));
        RECT r = { x, y, x + sz, y + sz };
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
}

void ApplyGlitch(HDC dc, int w, int h, int intensity) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);

    int patches = 5 + intensity / 4;
    for (int i = 0; i < patches; ++i) {
        int x = rand() % (w - 200);
        int y = rand() % (h - 100);
        int pw = 50 + rand() % 200;
        int ph = 20 + rand() % 80;
        int dx = (rand() % 31) - 15;
        int dy = (rand() % 11) - 5;
        BitBlt(dc, x + dx, y + dy, pw, ph, mem, x, y, SRCCOPY);
    }
    int inv = 2 + intensity / 10;
    for (int i = 0; i < inv; ++i) {
        int x = rand() % (w - 200);
        int y = rand() % (h - 100);
        RECT r = { x, y, x + 40 + rand()%150, y + 30 + rand()%80 };
        InvertRect(dc, &r);
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void ApplyInvert(HDC dc, int w, int h, int intensity) {
    if (intensity >= 90) {
        BitBlt(dc, 0, 0, w, h, dc, 0, 0, NOTSRCCOPY);
    } else {
        int count = 10 + intensity * 4;
        for (int i = 0; i < count; ++i) {
            int x = rand() % (w - 100);
            int y = rand() % (h - 100);
            RECT r = { x, y, x + 20 + rand()%80, y + 20 + rand()%80 };
            InvertRect(dc, &r);
        }
    }
}

void ApplyGrayscale(HDC dc, int w, int h, int intensity) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<unsigned char> px(w * h * 4);
    HBITMAP bmp = (HBITMAP)GetCurrentObject(dc, OBJ_BITMAP);
    GetDIBits(dc, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);

    double blend = intensity / 100.0;
    for (int i = 0; i < w * h; ++i) {
        int b = px[i*4+0], g = px[i*4+1], r = px[i*4+2];
        int gray = (r * 30 + g * 59 + b * 11) / 100;
        px[i*4+0] = (unsigned char)(b + (gray - b) * blend);
        px[i*4+1] = (unsigned char)(g + (gray - g) * blend);
        px[i*4+2] = (unsigned char)(r + (gray - r) * blend);
    }
    SetDIBits(dc, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
}

void ApplyKaleidoscope(HDC dc, int w, int h, int intensity) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);

    int QW = w / 2, QH = h / 2;
    double cx = w / 2.0, cy = h / 2.0;

    HBRUSH black = CreateSolidBrush(RGB(0,0,0));
    RECT full = { 0, 0, w, h };
    FillRect(dc, &full, black);
    DeleteObject(black);

    for (int q = 0; q < 4; ++q) {
        double a = g_kaleidoAngle + q * 1.5707963;
        double ca = cos(a), sa = sin(a);
        POINT pts[3];
        pts[0].x = (LONG)cx;
        pts[0].y = (LONG)cy;
        pts[1].x = (LONG)(cx + QW * ca);
        pts[1].y = (LONG)(cy + QW * sa);
        pts[2].x = (LONG)(cx - QH * sa);
        pts[2].y = (LONG)(cy + QH * ca);
        PlgBlt(dc, pts, mem, 0, 0, QW, QH, nullptr, 0, 0);
    }
    g_kaleidoAngle += 0.01 + intensity / 5000.0;

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void ApplyRotate(HDC dc, int w, int h, int intensity) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);

    HBRUSH black = CreateSolidBrush(RGB(0,0,0));
    RECT full = { 0, 0, w, h };
    FillRect(dc, &full, black);
    DeleteObject(black);

    double cx = w / 2.0, cy = h / 2.0;
    double ca = cos(g_rotateAngle), sa = sin(g_rotateAngle);

    POINT pt[3];
    pt[0].x = (LONG)(cx + (0 - cx)*ca - (0 - cy)*sa);
    pt[0].y = (LONG)(cy + (0 - cx)*sa + (0 - cy)*ca);
    pt[1].x = (LONG)(cx + (w - cx)*ca - (0 - cy)*sa);
    pt[1].y = (LONG)(cy + (w - cx)*sa + (0 - cy)*ca);
    pt[2].x = (LONG)(cx + (0 - cx)*ca - (h - cy)*sa);
    pt[2].y = (LONG)(cy + (0 - cx)*sa + (h - cy)*ca);

    PlgBlt(dc, pt, mem, 0, 0, w, h, nullptr, 0, 0);
    g_rotateAngle += 0.005 + intensity / 10000.0;

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void ApplyTear(HDC dc, int w, int h, int intensity) {
    int strips = 20 + intensity / 3;
    int stripH = h / strips;
    if (stripH < 1) stripH = 1;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);

    for (int i = 0; i < strips; ++i) {
        int y = i * stripH;
        int off = (rand() % (intensity + 1)) - intensity / 2;
        BitBlt(dc, off, y, w, stripH, mem, 0, y, SRCCOPY);
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void ApplyZoom(HDC dc, int w, int h, int intensity) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);

    POINT cur;
    GetCursorPos(&cur);
    int cx = (int)((double)cur.x / g_sw * w);
    int cy = (int)((double)cur.y / g_sh * h);

    HBRUSH black = CreateSolidBrush(RGB(0,0,0));
    RECT full = { 0, 0, w, h };
    FillRect(dc, &full, black);
    DeleteObject(black);

    int copies = 4 + intensity / 8;
    for (int i = 0; i < copies; ++i) {
        double scale = 1.0 - i * (0.05 + intensity / 1000.0);
        if (scale < 0.2) break;
        int nw = (int)(w * scale);
        int nh = (int)(h * scale);
        int nx = cx - (int)(cx * scale);
        int ny = cy - (int)(cy * scale);

        HDC tmp = CreateCompatibleDC(dc);
        HBITMAP tb = CreateCompatibleBitmap(dc, nw, nh);
        HBITMAP to = (HBITMAP)SelectObject(tmp, tb);
        SetStretchBltMode(tmp, HALFTONE);
        StretchBlt(tmp, 0, 0, nw, nh, mem, 0, 0, w, h, SRCCOPY);

        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 60, 0 };
        AlphaBlend(dc, nx, ny, nw, nh, tmp, 0, 0, nw, nh, bf);

        SelectObject(tmp, to);
        DeleteObject(tb);
        DeleteDC(tmp);
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void ApplyMatrix(HDC dc, int w, int h, int intensity) {
    const int CW = 14;
    int cols = w / CW;
    if ((int)g_matrixDrops.size() != cols) {
        g_matrixDrops.resize(cols);
        for (int i = 0; i < cols; ++i) {
            g_matrixDrops[i].x = i * CW;
            g_matrixDrops[i].y = -rand() % h;
        }
    }

    HFONT font = CreateFontW(16, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, 0, 0, L"Consolas");
    HFONT oldF = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    const wchar_t* glyphs = L"01";
    for (int i = 0; i < cols; ++i) {
        int x = g_matrixDrops[i].x;
        int y = g_matrixDrops[i].y;

        SetTextColor(dc, RGB(200, 255, 200));
        wchar_t c = glyphs[rand() % 2];
        TextOutW(dc, x, y, &c, 1);

        for (int t = 1; t < 12; ++t) {
            int ty = y - t * 18;
            if (ty < 0 || ty > h) continue;
            int alpha = 255 - t * 20;
            if (alpha < 0) alpha = 0;
            SetTextColor(dc, RGB(0, alpha, 0));
            wchar_t tc = glyphs[rand() % 2];
            TextOutW(dc, x, ty, &tc, 1);
        }

        g_matrixDrops[i].y += 4 + rand() % 5 + intensity / 20;
        if (g_matrixDrops[i].y > h + 200) {
            g_matrixDrops[i].y = -rand() % 300;
        }
    }

    SelectObject(dc, oldF);
    DeleteObject(font);
}

// ============================================================
// RENDER LOOP
// ============================================================
void RenderLoop() {
    const int BW = 960, BH = 540;

    HDC sdc = GetDC(nullptr);
    HDC workDC = CreateCompatibleDC(sdc);
    HBITMAP workBmp = CreateCompatibleBitmap(sdc, BW, BH);
    HBITMAP oldWB = (HBITMAP)SelectObject(workDC, workBmp);
    ReleaseDC(nullptr, sdc);

    int frameCount = 0;
    DWORD lastFpsTime = GetTickCount();

    while (g_renderThreadRun.load()) {
        if (!g_running.load()) {
            Sleep(50);
            continue;
        }

        DWORD frameStart = GetTickCount();

        HDC screen = GetDC(nullptr);
        SetStretchBltMode(workDC, COLORONCOLOR);
        StretchBlt(workDC, 0, 0, BW, BH, screen, 0, 0, g_sw, g_sh, SRCCOPY);
        ReleaseDC(nullptr, screen);

        int intensity = g_intensity.load();

        if (g_fxOn[FX_GRAYSCALE].load()) ApplyGrayscale(workDC, BW, BH, intensity);
        if (g_fxOn[FX_SCANLINES].load()) ApplyScanlines(workDC, BW, BH, intensity);
        if (g_fxOn[FX_INVERT].load())    ApplyInvert(workDC, BW, BH, intensity);
        if (g_fxOn[FX_TEAR].load())      ApplyTear(workDC, BW, BH, intensity);
        if (g_fxOn[FX_GLITCH].load())    ApplyGlitch(workDC, BW, BH, intensity);
        if (g_fxOn[FX_KALEIDO].load())   ApplyKaleidoscope(workDC, BW, BH, intensity);
        if (g_fxOn[FX_ROTATE].load())    ApplyRotate(workDC, BW, BH, intensity);
        if (g_fxOn[FX_ZOOM].load())      ApplyZoom(workDC, BW, BH, intensity);
        if (g_fxOn[FX_MATRIX].load())    ApplyMatrix(workDC, BW, BH, intensity);
        if (g_fxOn[FX_NOISE].load())     ApplyNoise(workDC, BW, BH, intensity);

        if (g_overlay) {
            HDC odc = GetDC(g_overlay);
            SetStretchBltMode(odc, COLORONCOLOR);
            StretchBlt(odc, 0, 0, g_sw, g_sh, workDC, 0, 0, BW, BH, SRCCOPY);
            ReleaseDC(g_overlay, odc);
        }

        frameCount++;
        DWORD now = GetTickCount();
        if (now - lastFpsTime >= 1000) {
            g_measuredFps = frameCount;
            frameCount = 0;
            lastFpsTime = now;
            if (g_fpsDisplay) {
                wchar_t buf[64];
                swprintf_s(buf, L"FPS: %d", g_measuredFps.load());
                SetWindowTextW(g_fpsDisplay, buf);
            }
        }

        int targetMs = 1000 / g_targetFps.load();
        int elapsed = (int)(now - frameStart);
        if (elapsed < targetMs) Sleep(targetMs - elapsed);
    }

    SelectObject(workDC, oldWB);
    DeleteObject(workBmp);
    DeleteDC(workDC);
}

// ============================================================
// OVERLAY WINDOW
// ============================================================
LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ============================================================
// PANEL WINDOW
// ============================================================
LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        int y = 15;
        for (int i = 0; i < FX_COUNT; ++i) {
            g_checkboxes[i] = CreateWindowExW(0, L"BUTTON", g_fxNames[i],
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                15, y, 340, 22, h,
                (HMENU)(INT_PTR)(ID_CHECKBOX_BASE + i), g_hInst, nullptr);
            y += 24;
        }

        y += 10;
        g_intensityLabel = CreateWindowExW(0, L"STATIC", L"Intensity: 50",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            15, y, 340, 20, h, nullptr, g_hInst, nullptr);
        y += 22;
        g_intensitySlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
            15, y, 340, 28, h,
            (HMENU)ID_INTENSITY, g_hInst, nullptr);
        SendMessageW(g_intensitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(g_intensitySlider, TBM_SETPOS, TRUE, 50);
        y += 40;

        g_fpsLabel = CreateWindowExW(0, L"STATIC", L"Target FPS: 30",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            15, y, 340, 20, h, nullptr, g_hInst, nullptr);
        y += 22;
        g_fpsSlider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
            15, y, 340, 28, h,
            (HMENU)ID_FPS_SLIDER, g_hInst, nullptr);
        SendMessageW(g_fpsSlider, TBM_SETRANGE, TRUE, MAKELPARAM(5, 60));
        SendMessageW(g_fpsSlider, TBM_SETPOS, TRUE, 30);
        y += 40;

        g_startBtn = CreateWindowExW(0, L"BUTTON", L"\u25B6 START",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            15, y, 340, 40, h,
            (HMENU)ID_START, g_hInst, nullptr);
        y += 50;

        g_fpsDisplay = CreateWindowExW(0, L"STATIC", L"FPS: 0",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            15, y, 340, 24, h, nullptr, g_hInst, nullptr);

        return 0;
    }

    case WM_HSCROLL: {
        HWND ctrl = (HWND)l;
        if (ctrl == g_intensitySlider) {
            int val = (int)SendMessageW(g_intensitySlider, TBM_GETPOS, 0, 0);
            g_intensity = val;
            wchar_t buf[64];
            swprintf_s(buf, L"Intensity: %d", val);
            SetWindowTextW(g_intensityLabel, buf);
        } else if (ctrl == g_fpsSlider) {
            int val = (int)SendMessageW(g_fpsSlider, TBM_GETPOS, 0, 0);
            g_targetFps = val;
            wchar_t buf[64];
            swprintf_s(buf, L"Target FPS: %d", val);
            SetWindowTextW(g_fpsLabel, buf);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_START) {
            bool running = !g_running.load();
            g_running = running;
            SetWindowTextW(g_startBtn, running ? L"\u25A0 STOP" : L"\u25B6 START");
            if (running) {
                ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
            } else {
                ShowWindow(g_overlay, SW_HIDE);
            }
        } else if (id >= ID_CHECKBOX_BASE && id < ID_CHECKBOX_BASE + FX_COUNT) {
            int idx = id - ID_CHECKBOX_BASE;
            bool checked = (SendMessageW(g_checkboxes[idx], BM_GETCHECK, 0, 0) == BST_CHECKED);
            g_fxOn[idx] = checked;
        }
        return 0;
    }

    case WM_HOTKEY:
        if (w == HOTKEY_TOGGLE) {
            if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
            else { ShowWindow(h, SW_SHOW); SetForegroundWindow(h); }
        }
        return 0;

    case WM_CLOSE:
        g_renderThreadRun = false;
        if (g_renderThread.joinable()) g_renderThread.join();
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ============================================================
// ТОЧКА ВХОДА
// ============================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_hInst = hInst;
    srand((unsigned)time(nullptr));

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);

    for (int i = 0; i < FX_COUNT; ++i) g_fxOn[i] = false;

    WNDCLASSW wc = {};
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    wc.lpfnWndProc   = PanelProc;
    wc.lpszClassName = PANEL_CLASS;
    RegisterClassW(&wc);

    wc.lpfnWndProc   = OverlayProc;
    wc.lpszClassName = OVERLAY_CLASS;
    wc.hbrBackground = nullptr;
    wc.hCursor       = nullptr;
    RegisterClassW(&wc);

    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS, L"", WS_POPUP,
        0, 0, g_sw, g_sh,
        nullptr, nullptr, hInst, nullptr);

    int panelW = 380, panelH = 620;
    int px = g_sw - panelW - 20;
    int py = 40;

    g_panel = CreateWindowExW(
        WS_EX_TOPMOST,
        PANEL_CLASS, L"GDI Control Panel",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        px, py, panelW, panelH,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_panel, SW_SHOW);
    UpdateWindow(g_panel);

    RegisterHotKey(g_panel, HOTKEY_TOGGLE, MOD_CONTROL | MOD_ALT, 'G');

    g_renderThreadRun = true;
    g_renderThread = std::thread(RenderLoop);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_renderThreadRun = false;
    if (g_renderThread.joinable()) g_renderThread.join();

    if (g_overlay) DestroyWindow(g_overlay);

    return 0;
}
