// ============================================================
//  Drift Handling Editor — SAMP ASI Plugin
//  Compatible: GTA SA v1.0 US + SAMPFUNCS
//
//  BUILD INSTRUCTIONS (bottom of file)
//  INSTALL: copy drift_handling_editor.asi to
//           GTA San Andreas/SAMPFUNCS/scripts/
//
//  CONTROLS:
//    F5        — open / close menu
//    F6        — move selection UP
//    F7        — move selection DOWN
//    F8        — decrease selected value
//    F9        — increase selected value
//    F10       — restore original values (stock)
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>

// ============================================================
//  GTA SA v1.0 US — Memory Addresses
// ============================================================
static constexpr uintptr_t ADDR_VEHICLE_PTR = 0xBA18FC;  // pointer to local player's current vehicle
static constexpr uintptr_t ADDR_GAME_STATE  = 0xC8D4C0;  // 9 = in-game

// CVehicle offset to tHandlingData*
static constexpr uintptr_t OFF_HANDLING_PTR = 0x04A4;

// ============================================================
//  tHandlingData field offsets (SA SDK verified, v1.0 US)
// ============================================================
static constexpr uintptr_t H_MAX_SPEED      = 0x08;  // float — max speed (m/s, 0.875 ~ 130 kph)
static constexpr uintptr_t H_ENGINE_ACCEL   = 0x0C;  // float — engine acceleration
static constexpr uintptr_t H_TRACTION_MULT  = 0x18;  // float — traction multiplier
static constexpr uintptr_t H_TRACTION_LOSS  = 0x1C;  // float — traction loss (higher = more slide)
static constexpr uintptr_t H_TRACTION_BIAS  = 0x20;  // float — traction bias (0=rear, 1=front)
static constexpr uintptr_t H_DRAG_MULT      = 0x28;  // float — drag multiplier

// ============================================================
//  Field Descriptor
// ============================================================
struct Field {
    const char* label;
    uintptr_t   offset;
    float       step;
    float       minVal;
    float       maxVal;
};

static const Field FIELDS[] = {
    { "Max Speed",           H_MAX_SPEED,     0.025f,  0.10f,  5.00f },
    { "Engine Acceleration", H_ENGINE_ACCEL,  0.500f,  1.00f, 200.0f },
    { "Traction Multiplier", H_TRACTION_MULT, 0.050f,  0.10f,  5.00f },
    { "Traction Loss",       H_TRACTION_LOSS, 0.050f,  0.10f,  3.00f },
    { "Drag Multiplier",     H_DRAG_MULT,     0.100f,  0.10f, 10.00f },
};
static constexpr int FIELD_COUNT = 5;

// Saved original values (read once on first menu open per vehicle entry)
static float g_origValues[FIELD_COUNT] = {};
static bool  g_origSaved = false;

// ============================================================
//  Memory Helpers
// ============================================================
static uintptr_t SafeReadPtr(uintptr_t addr) {
    __try {
        return *reinterpret_cast<uintptr_t*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static float SafeReadFloat(uintptr_t base, uintptr_t offset) {
    __try {
        return *reinterpret_cast<float*>(base + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}

static void SafeWriteFloat(uintptr_t base, uintptr_t offset, float value) {
    __try {
        *reinterpret_cast<float*>(base + offset) = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool IsInGame() {
    __try {
        return *reinterpret_cast<uint8_t*>(ADDR_GAME_STATE) == 9;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uintptr_t GetHandlingPtr() {
    uintptr_t veh = SafeReadPtr(ADDR_VEHICLE_PTR);
    if (!veh) return 0;
    return SafeReadPtr(veh + OFF_HANDLING_PTR);
}

// ============================================================
//  Clamp helper
// ============================================================
static float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ============================================================
//  UI State
// ============================================================
static bool g_menuOpen    = false;
static int  g_selectedRow = 0;

// Edge detection state for F5–F10
static bool g_prevKey[6] = {};
// Key indices: 0=F5 1=F6 2=F7 3=F8 4=F9 5=F10
static const int VKEYS[6] = { VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10 };

static bool KeyEdge(int idx) {
    bool cur = (GetAsyncKeyState(VKEYS[idx]) & 0x8000) != 0;
    bool edge = cur && !g_prevKey[idx];
    g_prevKey[idx] = cur;
    return edge;
}

// ============================================================
//  Float → string (3 decimal places, no sprintf/CRT)
// ============================================================
static void FloatToStr(float v, char* buf, int bufSize) {
    bool neg = v < 0.0f;
    if (neg) v = -v;
    int whole = (int)v;
    int frac  = (int)((v - (float)whole) * 1000.0f + 0.5f);
    if (frac >= 1000) { whole++; frac = 0; }
    if (neg)
        wsprintfA(buf, "-%d.%03d", whole, frac);
    else
        wsprintfA(buf, "%d.%03d", whole, frac);
}

// ============================================================
//  GDI Drawing Helpers
// ============================================================
static HWND g_wnd  = nullptr;
static HFONT g_font = nullptr;

static void EnsureWindow() {
    if (!g_wnd || !IsWindow(g_wnd))
        g_wnd = FindWindowA("Grand theft auto San Andreas", nullptr);
}

static void DrawFilledRect(HDC hdc, int x, int y, int w, int h, COLORREF col) {
    HBRUSH br = CreateSolidBrush(col);
    RECT r = { x, y, x + w, y + h };
    FillRect(hdc, &r, br);
    DeleteObject(br);
}

static void DrawBorder(HDC hdc, int x, int y, int w, int h, COLORREF col) {
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, x,     y,     nullptr);
    LineTo  (hdc, x+w-1, y);
    LineTo  (hdc, x+w-1, y+h-1);
    LineTo  (hdc, x,     y+h-1);
    LineTo  (hdc, x,     y);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void DrawText2(HDC hdc, int x, int y, const char* str, COLORREF col) {
    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    TextOutA(hdc, x, y, str, lstrlenA(str));
}

// ============================================================
//  Menu Render
// ============================================================
static void RenderMenu(HDC hdc) {
    uintptr_t hPtr = GetHandlingPtr();

    // Save originals once per vehicle session
    if (!g_origSaved && hPtr) {
        for (int i = 0; i < FIELD_COUNT; i++)
            g_origValues[i] = SafeReadFloat(hPtr, FIELDS[i].offset);
        g_origSaved = true;
    }

    // Layout
    const int PX = 40, PY = 44;
    const int PW = 460;
    const int ROW_H = 22;
    const int HEADER_H = 20;
    const int FOOTER_H = 22;
    const int PH = HEADER_H + 18 + FIELD_COUNT * ROW_H + FOOTER_H + 8;

    // Background
    DrawFilledRect(hdc, PX, PY, PW, PH, RGB(15, 18, 32));
    DrawBorder    (hdc, PX, PY, PW, PH, RGB(70, 110, 200));

    // Title bar
    DrawFilledRect(hdc, PX+1, PY+1, PW-2, HEADER_H, RGB(35, 55, 120));
    DrawText2(hdc, PX+8, PY+3,
              "Drift Handling Editor v1.0  |  NewLife Roleplay",
              RGB(180, 210, 255));

    // Column headers
    int hy = PY + HEADER_H + 4;
    DrawText2(hdc, PX+8,   hy, "Field",         RGB(130, 140, 190));
    DrawText2(hdc, PX+280, hy, "Current",       RGB(130, 140, 190));
    DrawText2(hdc, PX+360, hy, "Default",       RGB(100, 110, 160));

    // Separator line
    DrawFilledRect(hdc, PX+4, hy+16, PW-8, 1, RGB(50, 60, 100));

    // Rows
    for (int i = 0; i < FIELD_COUNT; i++) {
        int ry = PY + HEADER_H + 22 + i * ROW_H;
        bool sel = (i == g_selectedRow);

        if (sel)
            DrawFilledRect(hdc, PX+2, ry-2, PW-4, ROW_H, RGB(35, 70, 150));

        // Arrow indicator
        if (sel)
            DrawText2(hdc, PX+2, ry, ">", RGB(255, 220, 0));

        // Label
        COLORREF lc = sel ? RGB(255, 255, 100) : RGB(200, 210, 230);
        DrawText2(hdc, PX+14, ry, FIELDS[i].label, lc);

        // Current value
        char vbuf[32] = "N/A";
        if (hPtr) {
            float cur = SafeReadFloat(hPtr, FIELDS[i].offset);
            FloatToStr(cur, vbuf, sizeof(vbuf));
        }
        COLORREF vc = sel ? RGB(80, 255, 120) : RGB(160, 230, 160);
        DrawText2(hdc, PX+280, ry, vbuf, vc);

        // Original value
        char obuf[32];
        FloatToStr(g_origValues[i], obuf, sizeof(obuf));
        DrawText2(hdc, PX+360, ry, obuf, RGB(120, 120, 160));
    }

    // Footer
    int fy = PY + PH - FOOTER_H + 2;
    DrawFilledRect(hdc, PX+1, fy-2, PW-2, 1, RGB(40, 50, 90));

    const char* hint = !hPtr
        ? "  Enter a vehicle first."
        : "  F6/F7 Navigate   F8 Decrease   F9 Increase   F10 Restore Stock   F5 Close";
    DrawText2(hdc, PX+4, fy+2, hint, RGB(100, 110, 150));
}

// ============================================================
//  Input Handler
// ============================================================
static void PollInput() {
    // F5 — toggle menu
    if (KeyEdge(0)) {
        g_menuOpen = !g_menuOpen;
        if (g_menuOpen) g_origSaved = false; // re-snapshot on open
    }

    if (!g_menuOpen) return;

    // F6 — up
    if (KeyEdge(1))
        g_selectedRow = (g_selectedRow - 1 + FIELD_COUNT) % FIELD_COUNT;

    // F7 — down
    if (KeyEdge(2))
        g_selectedRow = (g_selectedRow + 1) % FIELD_COUNT;

    uintptr_t hPtr = GetHandlingPtr();
    if (!hPtr) return;

    const Field& f = FIELDS[g_selectedRow];
    float cur = SafeReadFloat(hPtr, f.offset);

    // F8 — decrease
    if (KeyEdge(3))
        SafeWriteFloat(hPtr, f.offset, Clamp(cur - f.step, f.minVal, f.maxVal));

    // F9 — increase
    if (KeyEdge(4))
        SafeWriteFloat(hPtr, f.offset, Clamp(cur + f.step, f.minVal, f.maxVal));

    // F10 — restore originals
    if (KeyEdge(5) && g_origSaved) {
        for (int i = 0; i < FIELD_COUNT; i++)
            SafeWriteFloat(hPtr, FIELDS[i].offset, g_origValues[i]);
    }
}

// ============================================================
//  Main Loop Thread
// ============================================================
static DWORD WINAPI MainLoop(LPVOID) {
    // Wait for game to be fully loaded
    while (!IsInGame()) {
        Sleep(500);
        EnsureWindow();
    }
    Sleep(1500); // SAMP init settle time

    EnsureWindow();

    // Create font once
    g_font = CreateFontA(
        14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New"
    );

    while (true) {
        EnsureWindow();
        PollInput();

        if (g_menuOpen && g_wnd && IsInGame()) {
            HDC hdc = GetDC(g_wnd);
            if (hdc) {
                HFONT oldFont = (HFONT)SelectObject(hdc, g_font);
                RenderMenu(hdc);
                SelectObject(hdc, oldFont);
                ReleaseDC(g_wnd, hdc);
            }
        }

        Sleep(16); // ~60 fps poll
    }

    return 0;
}

// ============================================================
//  DLL Entry Point
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainLoop, nullptr, 0, nullptr);
    }
    return TRUE;
}

// ============================================================
//  BUILD INSTRUCTIONS
// ============================================================
//
//  OPTION A — Visual Studio (recommended):
//  ----------------------------------------
//  1. Open Visual Studio 2019 or 2022
//  2. File -> New Project -> "Dynamic-Link Library (DLL)" -> C++
//  3. Delete all default files, paste THIS file as the only .cpp
//  4. Project Properties:
//       Platform:          Win32  (NOT x64)
//       Configuration:     Release
//       Runtime Library:   Multi-threaded (/MT)  [no DLL dependency]
//       Subsystem:         Windows
//  5. Build -> Build Solution
//  6. Find output: Release/YourProjectName.dll
//  7. Rename to: drift_handling_editor.asi
//
//  OPTION B — MinGW (command line):
//  ----------------------------------------
//  g++ -shared -m32 -O2 -o drift_handling_editor.asi drift_handling_editor.cpp ^
//      -lkernel32 -luser32 -lgdi32 -static-libgcc -static-libstdc++
//
//  INSTALL:
//  ----------------------------------------
//  Copy drift_handling_editor.asi to:
//    GTA San Andreas/SAMPFUNCS/scripts/
//
//  REQUIREMENTS:
//  ----------------------------------------
//  - GTA San Andreas v1.0 US (downgrade if needed)
//  - ASI Loader (Silent's ASI Loader recommended)
//  - SAMPFUNCS v5.x installed
//  - SAMP 0.3.7 R1 or R3
// ============================================================
