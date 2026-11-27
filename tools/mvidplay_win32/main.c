/* SPDX-License-Identifier: Apache-2.0
 * mvidplay (Win32) — the Windows player. No SDL, no runtime: a 32-bit DIB
 * and StretchDIBits. Cross-compiled with x86_64-w64-mingw32-gcc.
 * keys: space pause, left/right step, home restart, esc quit
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mvid.h"

static mvid_t   g_v;
static uint8_t  g_sheet[MVID_MAX_TILES * 64];
static uint16_t g_map[128 * 128];
static uint16_t g_fb[320 * 240];
static uint32_t g_pix[320 * 240];
static int      g_w, g_h, g_paused;
static BITMAPINFO g_bi;

static void present(HWND h)
{
    mvid_draw(&g_v, g_fb, (unsigned)g_w, (unsigned)g_h);
    for (int i = 0; i < g_w * g_h; i++) {
        const uint16_t c = g_fb[i];
        const uint32_t r = (uint32_t)((c >> 11) & 31) * 255u / 31u;
        const uint32_t gg = (uint32_t)((c >> 5) & 63) * 255u / 63u;
        const uint32_t b = (uint32_t)(c & 31) * 255u / 31u;
        g_pix[i] = (r << 16) | (gg << 8) | b;
    }
    HDC dc = GetDC(h);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, g_w, g_h, 0, 0, g_w, g_h, g_pix, &g_bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(h, dc);
}

static LRESULT CALLBACK wnd(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_TIMER:
        if (!g_paused) {
            if (!mvid_next(&g_v)) {
                if (g_v.info.flags & MVID_F_LOOP) mvid_seek(&g_v, 0);
                else g_paused = 1;
            }
        }
        present(h);
        return 0;
    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE: DestroyWindow(h); return 0;
        case VK_SPACE: g_paused = !g_paused; return 0;
        case VK_HOME: mvid_seek(&g_v, 0); return 0;
        case VK_RIGHT: if (g_v.cur + 1 < g_v.info.frames) mvid_seek(&g_v, (uint16_t)(g_v.cur + 1)); return 0;
        case VK_LEFT: if (g_v.cur > 0) mvid_seek(&g_v, (uint16_t)(g_v.cur - 1)); return 0;
        default: return 0;
        }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)show;
    char path[MAX_PATH] = "clip.mvid";
    if (cmd && *cmd) {
        if (*cmd == '"') { const char *e = strchr(cmd + 1, '"'); if (e) { memcpy(path, cmd + 1, (size_t)(e - cmd - 1)); path[e - cmd - 1] = 0; } }
        else snprintf(path, sizeof(path), "%s", cmd);
    }
    size_t sz = 0;
    uint8_t *blob = mvid_read_file(path, &sz);
    if (!blob) { MessageBoxA(NULL, "cannot read clip", "mvidplay", MB_ICONERROR); return 2; }
    if (mvid_open(&g_v, blob, sz, g_sheet, g_map) != MVID_E_OK) {
        MessageBoxA(NULL, "not a valid MVID clip", "mvidplay", MB_ICONERROR); return 3;
    }
    g_w = g_v.info.width; g_h = g_v.info.height;
    g_bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bi.bmiHeader.biWidth = g_w;
    g_bi.bmiHeader.biHeight = -g_h;
    g_bi.bmiHeader.biPlanes = 1;
    g_bi.bmiHeader.biBitCount = 32;
    g_bi.bmiHeader.biCompression = BI_RGB;

    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd; wc.hInstance = inst; wc.lpszClassName = "mvidplay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    HWND h = CreateWindowA("mvidplay", path, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, g_w * 2 + 16, g_h * 2 + 39,
                           NULL, NULL, inst, NULL);
    SetTimer(h, 1, (UINT)(1000 / (g_v.info.fps ? g_v.info.fps : 12)), NULL);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    free(blob);
    return 0;
}
