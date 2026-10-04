#pragma execution_character_set("utf-8")
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <mmsystem.h>
#include <magnification.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "Magnification.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:wWinMainCRTStartup")

// ============================================================
// КОНФИГ
// ============================================================
namespace Cfg {
    constexpr int kNoiseMs       = 2000;
    constexpr int kGrayShakeMs   = 12000;
    constexpr int kKaleidoMs     = 15000;
    constexpr int kSlideMs       = 10000;
    constexpr int kThresholdMs   = 8000;
    constexpr int kZoomMs        = 3000;
    constexpr int kRotateMs      = 7000;
    constexpr int kTearMs        = 6000;
    constexpr int kErrorsFillMs  = 6000;
    constexpr int kCursorTrailMs = 8000;
    constexpr int kFinalMs       = 11000;
    constexpr int kMaxPrankMs    = 300000;
    constexpr int kErrCount      = 32;
}

// ============================================================
// ГЛОБАЛЫ
// ============================================================
std::atomic<bool> g_stop{false};

HHOOK       g_kbHook  = nullptr;
HWND        g_magWnd  = nullptr;
HINSTANCE   g_hInst   = nullptr;

std::atomic<int>  g_currentFormula{0};
std::atomic<int>  g_volume{100};
std::atomic<bool> g_audioStop{false};

const wchar_t* ERR_CLASS    = L"NitroErr";
const wchar_t* NOISE_CLASS  = L"NitroNoise";
const wchar_t* SLIDE_CLASS  = L"NitroSlide";

struct SlideData { HBITMAP bmp = nullptr; int offsetX = 0; int bmpW = 0, bmpH = 0; };
SlideData g_slideA, g_slideB;

HWND g_finalErrWnd = nullptr;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void PumpMessages();
void SleepPump(int ms);
bool IsElevated();
void RelaunchElevated();
LRESULT CALLBACK LLKeyboardProc(int, WPARAM, LPARAM);
void InstallKbHook();
void RemoveKbHook();
void DisableTaskManager();
void EnableTaskManager();
void ScheduleRestoreTaskManager();
void AudioThread();
DWORD BytebeatSample(int idx, DWORD t);
void InitMagnifier();
void SetColorEffect(const MAGCOLOREFFECT* e);
void ShowMagnifier(bool on);
void ShutdownMagnifier();
void ShakeScreen(int durationMs, int intensity);
LRESULT CALLBACK ErrProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK NoiseProc(HWND, UINT, WPARAM, LPARAM);
void RegisterErrClass();
void RegisterNoiseClass();
void ShowErrorWindow(int x, int y, int w, int h);
void ShowNoiseWindow();
void KaleidoscopeThread(int durationMs);
void SlideThread(int durationMs);
HBITMAP CaptureScreen();
void ThresholdThread(int durationMs);
void ZoomBlurThread(int durationMs);
void ScreenRotateThread(int durationMs);
void ScreenTearThread(int durationMs);
void ErrorsFillThread(int durationMs);
void CursorTrailThread(int durationMs);
void FinalPhase();
void MoveCursorTo(int tx, int ty, int durationMs);
void AutoClick();
void DoReboot();
void WatchdogThread();

// ============================================================
// УТИЛИТЫ
// ============================================================
void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void SleepPump(int ms) {
    DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < ms && !g_stop.load()) {
        PumpMessages();
        Sleep(15);
    }
}

// ============================================================
// ПРАВА
// ============================================================
bool IsElevated() {
    HANDLE hTok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok)) return false;
    TOKEN_ELEVATION el = {};
    DWORD sz = sizeof(el);
    bool ok = GetTokenInformation(hTok, TokenElevation, &el, sz, &sz)
              && el.TokenIsElevated;
    CloseHandle(hTok);
    return ok;
}

void RelaunchElevated() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow  = SW_SHOW;
    ShellExecuteExW(&sei);
}

// ============================================================
// КЛАВИАТУРА
// ============================================================
LRESULT CALLBACK LLKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
            wParam == WM_KEYUP   || wParam == WM_SYSKEYUP) {
            return 1;
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

void InstallKbHook() {
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyboardProc, nullptr, 0);
}
void RemoveKbHook() {
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
}

// ============================================================
// ДИСПЕТЧЕР ЗАДАЧ
// ============================================================
void DisableTaskManager() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD val = 1;
        RegSetValueExW(hKey, L"DisableTaskMgr", 0, REG_DWORD,
                       (const BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
    }
}

void EnableTaskManager() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD val = 0;
        RegSetValueExW(hKey, L"DisableTaskMgr", 0, REG_DWORD,
                       (const BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
    }
}

void ScheduleRestoreTaskManager() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        const wchar_t* cmd =
            L"reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" "
            L"/v DisableTaskMgr /t REG_DWORD /d 0 /f";
        RegSetValueExW(hKey, L"RestoreTaskMgr", 0, REG_SZ,
                       (const BYTE*)cmd,
                       (DWORD)((wcslen(cmd)+1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

// ============================================================
// BYTEBEAT
// ============================================================
DWORD BytebeatSample(int idx, DWORD t) {
    switch (idx) {
        case 0: return (((t >> 12) | (t >> 10)) & 0xFF);
        case 1: return (((t >> 8) | (t >> 9) | (t * (t >> 13))) & 0xFF);
        case 2: return (((t>>10 & 42) * t +
                         (t | t % 255 | t % 257) +
                         (t & t >> 8) +
                         ((t % ((t >> 8 | t >> 16) + 1)) ^ t)) & 0xFF);
        case 3: return ((((t >> 5) | t) * (t >> 10)) & 0xFF);
        case 4: return ((t ^ (t << 3)) & 0xFF);
        case 5: return (((t >> 14) | (t * (t >> 13))) & 0xFF);
        case 6: return (((t >> 6) * ((t >> 10) | 1)) & 0xFF);
        case 7: return (((t ^ (t >> 3)) * (t >> 8)) & 0xFF);
        case 8: return (((t ^ (t >> 4)) * (t | (t >> 8))) & 0xFF);
        case 9:
        default: return 128;
    }
}

void AudioThread() {
    const int SR = 15800;
    const int BUF_SAMPLES = 1024;
    const int NUM_BUFFERS = 4;

    WAVEFORMATEX wf = {};
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = SR;
    wf.wBitsPerSample  = 8;
    wf.nBlockAlign     = 1;
    wf.nAvgBytesPerSec = SR;
    wf.cbSize          = 0;

    HWAVEOUT hwo = nullptr;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL)
        != MMSYSERR_NOERROR) return;

    std::vector<unsigned char> samples(NUM_BUFFERS * BUF_SAMPLES);
    std::vector<WAVEHDR> headers(NUM_BUFFERS);

    for (int i = 0; i < NUM_BUFFERS; ++i) {
        headers[i] = {};
        headers[i].lpData         = (LPSTR)&samples[i * BUF_SAMPLES];
        headers[i].dwBufferLength = BUF_SAMPLES;
        waveOutPrepareHeader(hwo, &headers[i], sizeof(WAVEHDR));
    }

    DWORD t = 0;
    int curBuf = 0;
    while (!g_audioStop.load()) {
        int guard = 0;
        while (!(headers[curBuf].dwFlags & WHDR_DONE)) {
            if (g_audioStop.load()) goto cleanup;
            Sleep(2);
            if (++guard > 500) break;
        }

        int formulaIdx = g_currentFormula.load();
        int volume     = g_volume.load();
        unsigned char* buf = (unsigned char*)headers[curBuf].lpData;

        for (int i = 0; i < BUF_SAMPLES; ++i) {
            DWORD raw = BytebeatSample(formulaIdx, t++);
            int s = (int)raw - 128;
            s = (s * volume) / 100;
            if (s >  127) s =  127;
            if (s < -128) s = -128;
            buf[i] = (unsigned char)(s + 128);
        }

        headers[curBuf].dwFlags &= ~WHDR_DONE;
        headers[curBuf].dwBufferLength = BUF_SAMPLES;
        waveOutWrite(hwo, &headers[curBuf], sizeof(WAVEHDR));
        curBuf = (curBuf + 1) % NUM_BUFFERS;
    }

cleanup:
    waveOutReset(hwo);
    for (int i = 0; i < NUM_BUFFERS; ++i)
        waveOutUnprepareHeader(hwo, &headers[i], sizeof(WAVEHDR));
    waveOutClose(hwo);
}

// ============================================================
// MAGNIFIER
// ============================================================
MAGCOLOREFFECT g_grayEffect = {
    0.30f, 0.30f, 0.30f, 0.0f, 0.0f,
    0.59f, 0.59f, 0.59f, 0.0f, 0.0f,
    0.11f, 0.11f, 0.11f, 0.0f, 0.0f,
    0.0f,  0.0f,  0.0f,  1.0f, 0.0f,
    0.0f,  0.0f,  0.0f,  0.0f, 1.0f
};
MAGCOLOREFFECT g_identityEffect = {
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f, 1.0f
};

void InitMagnifier() {
    MagInitialize();
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    g_magWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        WC_MAGNIFIER, L"", WS_POPUP,
        0, 0, sw, sh, nullptr, nullptr, g_hInst, nullptr);
    if (g_magWnd) {
        ShowWindow(g_magWnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_magWnd, HWND_TOPMOST, 0, 0, sw, sh, SWP_NOACTIVATE);
        MAGTRANSFORM tr = { 1.0f,0.0f,0.0f, 0.0f,1.0f,0.0f, 0.0f,0.0f,1.0f };
        MagSetWindowTransform(g_magWnd, &tr);
        RECT src = { 0, 0, sw, sh };
        MagSetWindowSource(g_magWnd, src);
        MagSetColorEffect(g_magWnd, &g_identityEffect);
    }
}
void SetColorEffect(const MAGCOLOREFFECT* e) {
    if (g_magWnd) MagSetColorEffect(g_magWnd, (PMAGCOLOREFFECT)e);
}
void ShowMagnifier(bool on) {
    if (!g_magWnd) return;
    if (on) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(g_magWnd, HWND_TOPMOST, 0, 0, sw, sh, SWP_NOACTIVATE);
        MAGTRANSFORM tr = { 1.0f,0.0f,0.0f, 0.0f,1.0f,0.0f, 0.0f,0.0f,1.0f };
        MagSetWindowTransform(g_magWnd, &tr);
        RECT src = { 0, 0, sw, sh };
        MagSetWindowSource(g_magWnd, src);
        ShowWindow(g_magWnd, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(g_magWnd, SW_HIDE);
    }
}
void ShutdownMagnifier() {
    if (g_magWnd) { DestroyWindow(g_magWnd); g_magWnd = nullptr; }
    MagUninitialize();
}

// ============================================================
// ТРЯСКА
// ============================================================
void ShakeScreen(int durationMs, int intensity) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hScreen = GetDC(nullptr);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP bmp = CreateCompatibleBitmap(hScreen, sw, sh);
    HBITMAP old = (HBITMAP)SelectObject(hMem, bmp);

    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        BitBlt(hMem, 0, 0, sw, sh, hScreen, 0, 0, SRCCOPY);
        int dx = (rand() % (intensity * 2 + 1)) - intensity;
        int dy = (rand() % (intensity * 2 + 1)) - intensity;
        BitBlt(hScreen, dx, dy, sw, sh, hMem, 0, 0, SRCCOPY);
        Sleep(20);
    }
    SelectObject(hMem, old);
    DeleteObject(bmp);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
}

// ============================================================
// ОКНО ОШИБКИ (MessageBox-стиль)
// ============================================================
struct ErrState { bool isRed = false; int hoverOk = 0; };

LRESULT CALLBACK ErrProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ErrState* st = (ErrState*)GetWindowLongPtrW(h, GWLP_USERDATA);

    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(240, 240, 240));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        HICON hIcon = LoadIconW(nullptr, IDI_ERROR);
        DrawIconEx(dc, 18, 18, hIcon, 32, 32, 0, nullptr, DI_NORMAL);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, (st && st->isRed) ? RGB(160,0,0) : RGB(0,0,0));
        HFONT font = CreateFontW(15, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(dc, font);
        RECT tr = { 64, 22, rc.right - 16, 62 };
        DrawTextW(dc, L"эщкере))", -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(font);

        int bw = 88, bh = 26;
        int bx = rc.right  - bw - 12;
        int by = rc.bottom - bh - 12;
        RECT br = { bx, by, bx + bw, by + bh };

        HBRUSH btnBg = CreateSolidBrush(
            (st && st->hoverOk) ? RGB(229,241,251) : RGB(225,225,225));
        FillRect(dc, &br, btnBg);
        DeleteObject(btnBg);

        HBRUSH frame = CreateSolidBrush(RGB(173, 173, 173));
        FrameRect(dc, &br, frame);
        DeleteObject(frame);

        SetTextColor(dc, RGB(0, 0, 0));
        HFONT bf = CreateFontW(13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT ob = (HFONT)SelectObject(dc, bf);
        DrawTextW(dc, L"ОК", -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, ob);
        DeleteObject(bf);

        EndPaint(h, &ps);
        return 0;
    }

    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_TIMER) {
        if (st) { st->isRed = !st->isRed; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        RECT rc; GetClientRect(h, &rc);
        int mx = LOWORD(l), my = HIWORD(l);
        int bw = 88, bh = 26;
        int bx = rc.right - bw - 12;
        int by = rc.bottom - bh - 12;
        if (mx >= bx && mx <= bx + bw && my >= by && my <= by + bh)
            DestroyWindow(h);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        RECT rc; GetClientRect(h, &rc);
        int mx = LOWORD(l), my = HIWORD(l);
        int bw = 88, bh = 26;
        int bx = rc.right - bw - 12;
        int by = rc.bottom - bh - 12;
        int hov = (mx >= bx && mx <= bx + bw &&
                   my >= by && my <= by + bh) ? 1 : 0;
        if (st && st->hoverOk != hov) {
            st->hoverOk = hov; InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme) };
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = h;
        TrackMouseEvent(&tme);
        return 0;
    }
    if (m == WM_MOUSELEAVE) {
        if (st) { st->hoverOk = 0; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    if (m == WM_DESTROY) {
        KillTimer(h, 1);
        if (st) { delete st; SetWindowLongPtrW(h, GWLP_USERDATA, 0); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void RegisterErrClass() {
    WNDCLASSW wc = {};
    wc.hInstance     = g_hInst;
    wc.lpfnWndProc   = ErrProc;
    wc.lpszClassName = ERR_CLASS;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
}

void ShowErrorWindow(int x, int y, int w, int h) {
    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        ERR_CLASS, L"Ошибка",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        x, y, w, h, nullptr, nullptr, g_hInst, nullptr);
    if (!hw) return;

    ErrState* st = new ErrState();
    SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)st);
    SetTimer(hw, 1, 350, nullptr);
    SendMessageW(hw, WM_SETICON, ICON_SMALL,
                 (LPARAM)LoadIconW(nullptr, IDI_ERROR));
    SendMessageW(hw, WM_SETICON, ICON_BIG,
                 (LPARAM)LoadIconW(nullptr, IDI_ERROR));
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    UpdateWindow(hw);
}

// ============================================================
// ОКНО С ШУМОМ
// ============================================================
LRESULT CALLBACK NoiseProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        int ww = rc.right, hh = rc.bottom;
        if (ww <= 0 || hh <= 0) { EndPaint(h, &ps); return 0; }

        for (int y = 0; y < hh; y += 2) {
            for (int x = 0; x < ww; x += 2) {
                int v = rand() & 0xFF;
                HBRUSH b = CreateSolidBrush(RGB(v, v, v));
                RECT r = { x, y, x + 2, y + 2 };
                FillRect(dc, &r, b);
                DeleteObject(b);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_TIMER) { InvalidateRect(h, nullptr, FALSE); return 0; }
    if (m == WM_DESTROY) { KillTimer(h, 1); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void RegisterNoiseClass() {
    WNDCLASSW wc = {};
    wc.hInstance     = g_hInst;
    wc.lpfnWndProc   = NoiseProc;
    wc.lpszClassName = NOISE_CLASS;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
}

void ShowNoiseWindow() {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 420, h = 180;
    int x = (sw - w) / 2, y = (sh - h) / 2;

    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        NOISE_CLASS, L"Критическая ошибка",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        x, y, w, h, nullptr, nullptr, g_hInst, nullptr);
    if (!hw) return;
    SendMessageW(hw, WM_SETICON, ICON_SMALL,
                 (LPARAM)LoadIconW(nullptr, IDI_ERROR));
    SendMessageW(hw, WM_SETICON, ICON_BIG,
                 (LPARAM)LoadIconW(nullptr, IDI_ERROR));
    SetTimer(hw, 1, 50, nullptr);
    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);

    SleepPump(Cfg::kNoiseMs);
    DestroyWindow(hw);
    PumpMessages();
}

// ============================================================
// ЗАХВАТ ЭКРАНА
// ============================================================
HBITMAP CaptureScreen() {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hSrc = GetDC(nullptr);
    HDC hMem = CreateCompatibleDC(hSrc);
    HBITMAP bmp = CreateCompatibleBitmap(hSrc, sw, sh);
    HBITMAP old = (HBITMAP)SelectObject(hMem, bmp);
    BitBlt(hMem, 0, 0, sw, sh, hSrc, 0, 0, SRCCOPY);
    SelectObject(hMem, old);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hSrc);
    return bmp;
}

// ============================================================
// ФАЗА 2: KALEIDOSCOPE (оптимизированный)
// ============================================================
void KaleidoscopeThread(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    const int BW = 480, BH = 270;
    const int QW = BW / 2;
    const int QH = BH / 2;

    HDC hScreen = GetDC(nullptr);

    HDC hWork = CreateCompatibleDC(hScreen);
    HBITMAP bWork = CreateCompatibleBitmap(hScreen, BW, BH);
    HBITMAP oldW = (HBITMAP)SelectObject(hWork, bWork);

    HBITMAP bgBmp = CaptureScreen();
    HDC hBg = CreateCompatibleDC(hScreen);
    HBITMAP oldBg = (HBITMAP)SelectObject(hBg, bgBmp);

    HDC hOut = CreateCompatibleDC(hScreen);
    HBITMAP bOut = CreateCompatibleBitmap(hScreen, BW, BH);
    HBITMAP oldO = (HBITMAP)SelectObject(hOut, bOut);

    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));

    DWORD start = GetTickCount();
    double angle = 0.0;

    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        SetStretchBltMode(hWork, COLORONCOLOR);
        StretchBlt(hWork, 0, 0, BW, BH, hBg, 0, 0, sw, sh, SRCCOPY);

        RECT rcOut = { 0, 0, BW, BH };
        FillRect(hOut, &rcOut, black);

        double cx = BW / 2.0;
        double cy = BH / 2.0;

        for (int q = 0; q < 4; ++q) {
            double a = angle + q * 1.5707963;
            double ca = cos(a), sa = sin(a);

            POINT pts[3];
            pts[0].x = (LONG)cx;
            pts[0].y = (LONG)cy;
            pts[1].x = (LONG)(cx + QW * ca);
            pts[1].y = (LONG)(cy + QW * sa);
            pts[2].x = (LONG)(cx - QH * sa);
            pts[2].y = (LONG)(cy + QH * ca);

            PlgBlt(hOut, pts, hWork, 0, 0, QW, QH, nullptr, 0, 0);
        }

        SetStretchBltMode(hScreen, COLORONCOLOR);
        StretchBlt(hScreen, 0, 0, sw, sh, hOut, 0, 0, BW, BH, SRCCOPY);

        angle += 0.04;
        Sleep(33);
    }

    DeleteObject(black);
    SelectObject(hWork, oldW);
    DeleteObject(bWork);
    DeleteDC(hWork);
    SelectObject(hBg, oldBg);
    DeleteObject(bgBmp);
    DeleteDC(hBg);
    SelectObject(hOut, oldO);
    DeleteObject(bOut);
    DeleteDC(hOut);
    ReleaseDC(nullptr, hScreen);
}

// ============================================================
// SLIDE
// ============================================================
void DrawSlide(HDC dc, SlideData& sd, int x) {
    if (!sd.bmp) return;
    HDC hMem = CreateCompatibleDC(dc);
    HBITMAP old = (HBITMAP)SelectObject(hMem, sd.bmp);
    BitBlt(dc, x, 0, sd.bmpW, sd.bmpH, hMem, 0, 0, SRCCOPY);
    SelectObject(hMem, old);
    DeleteDC(hMem);
}

LRESULT CALLBACK SlideProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        DrawSlide(dc, g_slideB, 0);
        DrawSlide(dc, g_slideA, -g_slideA.offsetX);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

void RegisterSlideClass() {
    WNDCLASSW wc = {};
    wc.hInstance     = g_hInst;
    wc.lpfnWndProc   = SlideProc;
    wc.lpszClassName = SLIDE_CLASS;
    RegisterClassW(&wc);
}

void SlideThread(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    g_slideA.bmp = CaptureScreen();
    g_slideA.bmpW = sw; g_slideA.bmpH = sh; g_slideA.offsetX = 0;
    g_slideB.bmp = CaptureScreen();
    g_slideB.bmpW = sw; g_slideB.bmpH = sh;

    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        SLIDE_CLASS, L"", WS_POPUP, 0, 0, sw, sh,
        nullptr, nullptr, g_hInst, nullptr);
    ShowWindow(hw, SW_SHOWNOACTIVATE);

    DWORD start = GetTickCount();
    double speed = 0.5;
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        g_slideA.offsetX += (int)speed;
        if (g_slideA.offsetX > sw) g_slideA.offsetX = 0;
        speed *= 1.02;
        if (speed > 25.0) speed = 25.0;
        InvalidateRect(hw, nullptr, FALSE);
        UpdateWindow(hw);
        PumpMessages();
        Sleep(20);
    }

    DestroyWindow(hw);
    if (g_slideA.bmp) { DeleteObject(g_slideA.bmp); g_slideA.bmp = nullptr; }
    if (g_slideB.bmp) { DeleteObject(g_slideB.bmp); g_slideB.bmp = nullptr; }
}

// ============================================================
// THRESHOLD
// ============================================================
void ThresholdThread(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    HDC hScreen = GetDC(nullptr);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP bmp = CreateCompatibleBitmap(hScreen, sw, sh);
    HBITMAP old = (HBITMAP)SelectObject(hMem, bmp);

    std::vector<unsigned char> pixels(sw * sh * 4);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = sw;
    bi.bmiHeader.biHeight      = -sh;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        DWORD elapsed = GetTickCount() - start;
        double phase = (double)elapsed / 1500.0;
        int threshold = (int)(125 + 95 * sin(phase));
        if (threshold < 30)  threshold = 30;
        if (threshold > 220) threshold = 220;

        BitBlt(hMem, 0, 0, sw, sh, hScreen, 0, 0, SRCCOPY);
        GetDIBits(hMem, bmp, 0, sh, pixels.data(), &bi, DIB_RGB_COLORS);

        for (int i = 0; i < sw * sh; ++i) {
            int b = pixels[i*4+0];
            int g = pixels[i*4+1];
            int r = pixels[i*4+2];
            int gray = (r * 30 + g * 59 + b * 11) / 100;

            if (gray > threshold) {
                pixels[i*4+0] = 200;
                pixels[i*4+1] = 255;
                pixels[i*4+2] = 230;
            } else {
                pixels[i*4+0] = 0;
                pixels[i*4+1] = 8;
                pixels[i*4+2] = 5;
            }
        }

        SetDIBits(hMem, bmp, 0, sh, pixels.data(), &bi, DIB_RGB_COLORS);
        BitBlt(hScreen, 0, 0, sw, sh, hMem, 0, 0, SRCCOPY);

        Sleep(40);
    }

    SelectObject(hMem, old);
    DeleteObject(bmp);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
}

// ============================================================
// ZOOM BLUR
// ============================================================
void ZoomBlurThread(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hScreen = GetDC(nullptr);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP bmp = CreateCompatibleBitmap(hScreen, sw, sh);
    HBITMAP old = (HBITMAP)SelectObject(hMem, bmp);

    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        POINT cur;
        GetCursorPos(&cur);
        BitBlt(hMem, 0, 0, sw, sh, hScreen, 0, 0, SRCCOPY);

        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        RECT rc = { 0, 0, sw, sh };
        FillRect(hScreen, &rc, black);
        DeleteObject(black);

        SetStretchBltMode(hScreen, HALFTONE);
        for (int i = 0; i < 18; ++i) {
            double scale = 1.0 - i * 0.045;
            if (scale < 0.15) break;
            int w = (int)(sw * scale);
            int h = (int)(sh * scale);
            int x = cur.x - (int)((cur.x * scale));
            int y = cur.y - (int)((cur.y * scale));
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 30, 0 };
            HDC hTmp = CreateCompatibleDC(hScreen);
            HBITMAP bTmp = CreateCompatibleBitmap(hScreen, w, h);
            HBITMAP oTmp = (HBITMAP)SelectObject(hTmp, bTmp);
            StretchBlt(hTmp, 0, 0, w, h, hMem, 0, 0, sw, sh, SRCCOPY);
            AlphaBlend(hScreen, x, y, w, h, hTmp, 0, 0, w, h, bf);
            SelectObject(hTmp, oTmp);
            DeleteObject(bTmp);
            DeleteDC(hTmp);
        }
        Sleep(30);
    }

    SelectObject(hMem, old);
    DeleteObject(bmp);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
}

// ============================================================
// SCREEN ROTATE
// ============================================================
void ScreenRotateThread(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hScreen = GetDC(nullptr);
    HBITMAP bmp = CaptureScreen();
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP old = (HBITMAP)SelectObject(hMem, bmp);

    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    DWORD start = GetTickCount();
    double angle = 0.0;
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        RECT rc = { 0, 0, sw, sh };
        FillRect(hScreen, &rc, black);

        double cx = sw / 2.0, cy = sh / 2.0;
        double ca = cos(angle), sa = sin(angle);

        POINT pt[3];
        pt[0].x = (LONG)(cx + (0  - cx)*ca - (0  - cy)*sa);
        pt[0].y = (LONG)(cy + (0  - cx)*sa + (0  - cy)*ca);
        pt[1].x = (LONG)(cx + (sw - cx)*ca - (0  - cy)*sa);
        pt[1].y = (LONG)(cy + (sw - cx)*sa + (0  - cy)*ca);
        pt[2].x = (LONG)(cx + (0  - cx)*ca - (sh - cy)*sa);
        pt[2].y = (LONG)(cy + (0  - cx)*sa + (sh - cy)*ca);

        PlgBlt(hScreen, pt, hMem, 0, 0, sw, sh, nullptr, 0, 0);

        angle += 0.03;
        Sleep(30);
    }

    SelectObject(hMem, old);
    DeleteObject(bmp);
    DeleteDC(hMem);
    DeleteObject(black);
    ReleaseDC(nullptr, hScreen);
}

// ============================================================
// SCREEN TEARING
// ============================================================
void ScreenTearThread(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hScreen = GetDC(nullptr);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP bmp = CreateCompatibleBitmap(hScreen, sw, sh);
    HBITMAP old = (HBITMAP)SelectObject(hMem, bmp);

    const int STRIPS = 40;
    int stripH = sh / STRIPS;
    std::vector<int> offs(STRIPS, 0);
    std::vector<int> spd(STRIPS);
    for (int i = 0; i < STRIPS; ++i)
        spd[i] = (rand() % 2 ? 1 : -1) * (3 + rand() % 12);

    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        BitBlt(hMem, 0, 0, sw, sh, hScreen, 0, 0, SRCCOPY);
        for (int i = 0; i < STRIPS; ++i) {
            int y = i * stripH;
            offs[i] += spd[i];
            if (abs(offs[i]) > sw / 2) spd[i] = -spd[i];
            BitBlt(hScreen, offs[i], y, sw, stripH, hMem, 0, y, SRCCOPY);
        }
        Sleep(50);
    }

    SelectObject(hMem, old);
    DeleteObject(bmp);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
}

// ============================================================
// 32 ОКНА ОШИБОК
// ============================================================
void ErrorsFillThread(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = sw / 8, wh = sh / 4;
    int cols = sw / ww, rows = sh / wh;
    int total = cols * rows;
    if (total > Cfg::kErrCount) total = Cfg::kErrCount;

    int perMs = durationMs / (total > 0 ? total : 1);
    for (int i = 0; i < total && !g_stop.load(); ++i) {
        int cx = (i % cols) * ww;
        int cy = (i / cols) * wh;
        ShowErrorWindow(cx, cy, ww, wh);
        SleepPump(perMs);
    }
}

// ============================================================
// ХВОСТ ОШИБОК ЗА КУРСОРОМ (8 секунд)
// ============================================================
void CursorTrailThread(int durationMs) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hScreen = GetDC(nullptr);

    HBITMAP bg = CaptureScreen();
    HDC hBg = CreateCompatibleDC(hScreen);
    HBITMAP oldBg = (HBITMAP)SelectObject(hBg, bg);

    HICON hIcon = LoadIconW(nullptr, IDI_ERROR);
    std::vector<POINT> trail;
    trail.reserve(300);

    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < durationMs) {
        POINT cur;
        GetCursorPos(&cur);
        trail.push_back(cur);
        if (trail.size() > 180) trail.erase(trail.begin());

        BitBlt(hScreen, 0, 0, sw, sh, hBg, 0, 0, SRCCOPY);

        for (size_t i = 0; i < trail.size(); ++i) {
            double t = (double)i / (double)trail.size();
            int sz = (int)(10 + t * 24);
            int cx = trail[i].x - sz / 2;
            int cy = trail[i].y - sz / 2;
            DrawIconEx(hScreen, cx, cy, hIcon, sz, sz, 0, nullptr, DI_NORMAL);
        }
        Sleep(25);
    }

    SelectObject(hBg, oldBg);
    DeleteObject(bg);
    DeleteDC(hBg);
    ReleaseDC(nullptr, hScreen);
}

// ============================================================
// АВТО-КУРСОР
// ============================================================
void MoveCursorTo(int tx, int ty, int durationMs) {
    POINT start;
    GetCursorPos(&start);
    DWORD t0 = GetTickCount();
    while (!g_stop.load()) {
        DWORD elapsed = GetTickCount() - t0;
        if ((int)elapsed >= durationMs) break;
        double t = (double)elapsed / (double)durationMs;
        double eased = 1.0 - pow(1.0 - t, 3.0);
        int x = (int)(start.x + (tx - start.x) * eased);
        int y = (int)(start.y + (ty - start.y) * eased);
        SetCursorPos(x, y);
        Sleep(8);
    }
    SetCursorPos(tx, ty);
}

void AutoClick() {
    INPUT inputs[2] = {};
    inputs[0].type           = INPUT_MOUSE;
    inputs[0].mi.dwFlags     = MOUSEEVENTF_LEFTDOWN;
    inputs[0].mi.dwExtraInfo = 0;
    inputs[1].type           = INPUT_MOUSE;
    inputs[1].mi.dwFlags     = MOUSEEVENTF_LEFTUP;
    inputs[1].mi.dwExtraInfo = 0;
    SendInput(2, inputs, sizeof(INPUT));
}

// ============================================================
// ФИНАЛ (фаза 9)
// ============================================================
void FinalPhase() {
    RedrawWindow(nullptr, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE);
    SleepPump(500);

    g_volume = 0;
    SleepPump(2000);

    g_volume = 80;
    g_currentFormula = 9;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 380, h = 180;
    int x = (sw - w) / 2;
    int y = (sh - h) / 2;

    g_finalErrWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        ERR_CLASS, L"Ошибка",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        x, y, w, h, nullptr, nullptr, g_hInst, nullptr);

    if (g_finalErrWnd) {
        ErrState* st = new ErrState();
        SetWindowLongPtrW(g_finalErrWnd, GWLP_USERDATA, (LONG_PTR)st);
        SetTimer(g_finalErrWnd, 1, 350, nullptr);
        SendMessageW(g_finalErrWnd, WM_SETICON, ICON_SMALL,
                     (LPARAM)LoadIconW(nullptr, IDI_ERROR));
        SendMessageW(g_finalErrWnd, WM_SETICON, ICON_BIG,
                     (LPARAM)LoadIconW(nullptr, IDI_ERROR));
        ShowWindow(g_finalErrWnd, SW_SHOW);
        UpdateWindow(g_finalErrWnd);
    }

    SleepPump(3000);

    if (g_finalErrWnd) {
        RECT wndRect;
        GetWindowRect(g_finalErrWnd, &wndRect);
        int wndW = wndRect.right - wndRect.left;
        int wndH = wndRect.bottom - wndRect.top;

        int okX = wndRect.left + wndW - 12 - 88 + 44;
        int okY = wndRect.top  + wndH - 12 - 26 - 22 + 13;

        MoveCursorTo(okX, okY, 2500);
        SleepPump(500);
        AutoClick();
        SleepPump(800);
    }

    DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < Cfg::kFinalMs && !g_stop.load()) {
        PumpMessages();
        Sleep(30);
    }
}

// ============================================================
// РЕБУТ
// ============================================================
void DoReboot() {
    HANDLE hTok;
    TOKEN_PRIVILEGES tkp;
    if (OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok)) {
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
        tkp.PrivilegeCount = 1;
        tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hTok, FALSE, &tkp, 0, nullptr, nullptr);
        CloseHandle(hTok);
    }
    ExitWindowsEx(EWX_REBOOT | EWX_FORCE, SHTDN_REASON_MAJOR_APPLICATION);
}

// ============================================================
// WATCHDOG
// ============================================================
void WatchdogThread() {
    DWORD start = GetTickCount();
    while (true) {
        if (g_stop.load()) return;
        if ((int)(GetTickCount() - start) > Cfg::kMaxPrankMs) {
            RemoveKbHook();
            g_audioStop = true;
            DoReboot();
            return;
        }
        Sleep(1000);
    }
}

// ============================================================
// ТОЧКА ВХОДА
// ============================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();
    g_hInst = hInst;

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"NitrogenPrankMutex_v3");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    if (!IsElevated()) {
        RelaunchElevated();
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 0;
    }

    srand((unsigned)time(nullptr));

    int r = MessageBoxW(nullptr,
        L"ВНИМАНИЕ!\n\n"
        L"Эта программа — БЕЗОБИДНЫЙ ПРАНК.\n"
        L"Она НЕ удалит ваши файлы, НЕ украдёт данные,\n"
        L"НЕ навредит вашему компьютеру/ноутбуку.\n\n"
        L"НО! Клавиатура будет временно заблокирована,\n"
        L"экран заполнится эффектами, а через ~90 секунд\n"
        L"произойдёт реальная перезагрузка.\n\n"
        L"СОХРАНИТЕ ВСЁ ОТКРЫТОЕ ПЕРЕД ЗАПУСКОМ!\n\n"
        L"Запустить?",
        L"nitrogen.exe — предупреждение",
        MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
    if (r != IDYES) {
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 0;
    }

    InstallKbHook();
    DisableTaskManager();
    ScheduleRestoreTaskManager();

    RegisterErrClass();
    RegisterNoiseClass();
    RegisterSlideClass();
    InitMagnifier();

    std::thread tWatch(WatchdogThread);
    std::thread tAudio(AudioThread);

    // ===== ФАЗА 0 =====
    g_currentFormula = 0;
    g_volume = 80;
    ShowNoiseWindow();

    // ===== ФАЗА 1 =====
    g_currentFormula = 1;
    g_volume = 120;
    SetColorEffect(&g_grayEffect);
    ShowMagnifier(true);
    ShakeScreen(Cfg::kGrayShakeMs, 2);
    ShowMagnifier(false);

    // ===== ФАЗА 2 (Kaleidoscope) =====
    g_currentFormula = 2;
    g_volume = 140;
    KaleidoscopeThread(Cfg::kKaleidoMs);

    // ===== ФАЗА 3 =====
    g_currentFormula = 3;
    g_volume = 120;
    SlideThread(Cfg::kSlideMs);

    // ===== ФАЗА 4 =====
    g_currentFormula = 4;
    g_volume = 130;
    ThresholdThread(Cfg::kThresholdMs);

    // ===== ФАЗА 5 =====
    g_currentFormula = 5;
    g_volume = 160;
    ZoomBlurThread(Cfg::kZoomMs);

    // ===== ФАЗА 6 =====
    g_currentFormula = 6;
    g_volume = 120;
    ScreenRotateThread(Cfg::kRotateMs);

    // ===== ФАЗА 7 =====
    g_currentFormula = 7;
    g_volume = 140;
    ScreenTearThread(Cfg::kTearMs);

    // ===== ФАЗА 8: сначала окна (6с), потом хвост (8с) =====
    g_currentFormula = 8;
    g_volume = 150;

    ErrorsFillThread(Cfg::kErrorsFillMs);

    CursorTrailThread(Cfg::kCursorTrailMs);

    {
        HWND hWnd = FindWindowW(ERR_CLASS, nullptr);
        while (hWnd) {
            DestroyWindow(hWnd);
            hWnd = FindWindowW(ERR_CLASS, nullptr);
        }
    }
    PumpMessages();

    // ===== ФАЗА 9 =====
    FinalPhase();

    // ===== КОНЕЦ =====
    RemoveKbHook();
    EnableTaskManager();

    g_audioStop = true;
    g_stop = true;
    if (tAudio.joinable()) tAudio.join();
    if (tWatch.joinable()) tWatch.join();

    ShutdownMagnifier();
    DoReboot();

    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return 0;
}
