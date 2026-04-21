// =============================================================================
//  PNG to CUR Converter with Hotspot Editor
//  A Windows GUI application to convert PNG images to .cur cursor files
//  and visually set the hotspot by clicking on the image.
//
//  COMPILE (MSVC Developer Command Prompt):
//    cl /EHsc /W3 /O2 cursel.cpp /link gdiplus.lib user32.lib gdi32.lib
//        comdlg32.lib comctl32.lib shell32.lib
//
//  COMPILE (MinGW / g++):
//    g++ -std=c++17 -O2 -o cursel.exe cursel.cpp
//        -lgdiplus -lcomdlg32 -lcomctl32 -lshell32 -mwindows
//
//  USAGE:
//    • File > Open PNG  (or drag & drop, or pass path as argument)
//    • Click on the image to place the hotspot crosshair
//    • Type exact pixel coords in X / Y fields and press Apply
//    • Scroll wheel to zoom in/out for precise placement
//    • File > Save as CUR
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

// GDI+ requires objbase.h (for IUnknown/PROPID) which WIN32_LEAN_AND_MEAN
// strips out. Include these in this exact order before <gdiplus.h>.
#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wtypes.h>
#include <gdiplus.h>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Gdiplus;

// ─── Control / Menu IDs ──────────────────────────────────────────────────────
#define IDC_CANVAS       1001
#define IDC_EDIT_X       1002
#define IDC_EDIT_Y       1003
#define IDC_BTN_OPEN     1004
#define IDC_BTN_SAVE     1005
#define IDC_BTN_APPLY    1006
#define IDC_BTN_CENTER   1007
#define IDC_STATUSBAR    1008

#define IDM_FILE_OPEN    2001
#define IDM_FILE_SAVE    2002
#define IDM_FILE_EXIT    2003
#define IDM_VIEW_ZOOMIN  2101
#define IDM_VIEW_ZOOMOUT 2102
#define IDM_VIEW_FIT     2103
#define IDM_VIEW_ACTUAL  2104
#define IDM_HELP_ABOUT   2201

#define TOOLBAR_H  46

// ─── CUR / ICO binary layout ─────────────────────────────────────────────────
#pragma pack(push, 1)
struct IconDir {
    WORD reserved;   // Must be 0
    WORD type;       // 1 = icon, 2 = cursor
    WORD count;      // Number of images
};
struct IconDirEntry {
    BYTE  width;         // 0 means 256
    BYTE  height;        // 0 means 256
    BYTE  colorCount;    // 0 for true-color
    BYTE  reserved;      // Must be 0
    WORD  xHotspot;      // CUR: x hotspot; ICO: color planes
    WORD  yHotspot;      // CUR: y hotspot; ICO: bits per pixel
    DWORD bytesInRes;    // Size of image data
    DWORD imageOffset;   // Offset from file start
};
#pragma pack(pop)

// ─── Global state ────────────────────────────────────────────────────────────
HINSTANCE    g_hInst       = nullptr;
HWND         g_hwnd        = nullptr;
HWND         g_hwndCanvas  = nullptr;
HWND         g_hwndStatus  = nullptr;
HWND         g_hwndEditX   = nullptr;
HWND         g_hwndEditY   = nullptr;
HWND         g_hwndBtnOpen = nullptr;
HWND         g_hwndBtnSave = nullptr;
HWND         g_hwndBtnApply= nullptr;
HWND         g_hwndBtnCtr  = nullptr;
ULONG_PTR    g_gdipToken   = 0;

Bitmap*      g_bmp         = nullptr;
std::wstring g_filePath;
int          g_imgW        = 0;
int          g_imgH        = 0;
int          g_hotX        = 0;
int          g_hotY        = 0;

float        g_zoom        = 1.0f;
int          g_offX        = 0;    // canvas pixel offset of image top-left
int          g_offY        = 0;
bool         g_dragging    = false;

// ─── Forward declarations ─────────────────────────────────────────────────────
LRESULT CALLBACK WndProc   (HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK CanvasProc(HWND, UINT, WPARAM, LPARAM);

void   DoOpenFile(HWND hwnd);
void   DoSaveFile(HWND hwnd);
bool   WriteCurFile(const std::wstring& path, Bitmap* bmp, int hotX, int hotY);
void   LoadBitmapFromPath(HWND hwnd, const std::wstring& path);
void   FitZoomToCanvas();
void   UpdateOffsets();
void   UpdateHotspotEdits();
void   ReadHotspotFromEdits();
void   UpdateStatus();
void   LayoutControls(int w, int h);
void   CanvasToImage(int cx, int cy, int& ix, int& iy);

// ─── CUR file writer ─────────────────────────────────────────────────────────
bool WriteCurFile(const std::wstring& path, Bitmap* bmp, int hotX, int hotY)
{
    int w = (int)bmp->GetWidth();
    int h = (int)bmp->GetHeight();
    if (w <= 0 || h <= 0) return false;

    // Lock bitmap bits; GDI+ PixelFormat32bppARGB = BGRA in memory (little-endian)
    BitmapData bd{};
    Rect rect(0, 0, w, h);
    if (bmp->LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
        return false;

    // DIBs are bottom-up, so flip the rows
    std::vector<DWORD> xorData((size_t)w * h);
    const BYTE* src = static_cast<const BYTE*>(bd.Scan0);
    for (int y = 0; y < h; ++y) {
        const DWORD* srcRow = reinterpret_cast<const DWORD*>(src + (size_t)y * bd.Stride);
        DWORD* dstRow = xorData.data() + (size_t)(h - 1 - y) * w;
        std::memcpy(dstRow, srcRow, (size_t)w * 4);
    }
    bmp->UnlockBits(&bd);

    // AND mask: 1-bpp, rows padded to 4 bytes, all 0 = alpha controls transparency
    int maskStride = ((w + 31) / 32) * 4;
    std::vector<BYTE> andMask((size_t)maskStride * h, 0);

    // BITMAPINFOHEADER — biHeight = 2*h to include both XOR + AND masks
    BITMAPINFOHEADER bih{};
    bih.biSize        = sizeof(BITMAPINFOHEADER);
    bih.biWidth       = w;
    bih.biHeight      = h * 2;
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;

    DWORD imgBytes = sizeof(BITMAPINFOHEADER)
                   + (DWORD)(w * h * 4)
                   + (DWORD)andMask.size();

    // CUR file header
    IconDir dir{ 0, 2, 1 };
    IconDirEntry entry{};
    entry.width       = (BYTE)(w == 256 ? 0 : w);
    entry.height      = (BYTE)(h == 256 ? 0 : h);
    entry.colorCount  = 0;
    entry.reserved    = 0;
    entry.xHotspot    = (WORD)hotX;
    entry.yHotspot    = (WORD)hotY;
    entry.bytesInRes  = imgBytes;
    entry.imageOffset = sizeof(IconDir) + sizeof(IconDirEntry);

    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;

    DWORD wr = 0;
    WriteFile(hf, &dir,           sizeof(dir),               &wr, nullptr);
    WriteFile(hf, &entry,         sizeof(entry),             &wr, nullptr);
    WriteFile(hf, &bih,           sizeof(bih),               &wr, nullptr);
    WriteFile(hf, xorData.data(), (DWORD)(w * h * 4),        &wr, nullptr);
    WriteFile(hf, andMask.data(), (DWORD)andMask.size(),     &wr, nullptr);
    CloseHandle(hf);
    return true;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────
void FitZoomToCanvas()
{
    if (!g_bmp || !g_hwndCanvas) return;
    RECT rc; GetClientRect(g_hwndCanvas, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return;
    float sx = (float)cw / g_imgW;
    float sy = (float)ch / g_imgH;
    g_zoom = std::min(sx, sy);
    // For tiny images (≤64px), never shrink below 1×
    if (g_imgW <= 64 && g_imgH <= 64)
        g_zoom = std::max(g_zoom, 1.0f);
    g_zoom = std::min(g_zoom, 8.0f);   // never start at ludicrous zoom
    UpdateOffsets();
}

void UpdateOffsets()
{
    if (!g_bmp || !g_hwndCanvas) return;
    RECT rc; GetClientRect(g_hwndCanvas, &rc);
    int dw = (int)(g_imgW * g_zoom);
    int dh = (int)(g_imgH * g_zoom);
    g_offX = std::max(0, ((int)rc.right  - dw) / 2);
    g_offY = std::max(0, ((int)rc.bottom - dh) / 2);
}

void CanvasToImage(int cx, int cy, int& ix, int& iy)
{
    ix = (int)((cx - g_offX) / g_zoom);
    iy = (int)((cy - g_offY) / g_zoom);
    ix = std::max(0, std::min(ix, g_imgW - 1));
    iy = std::max(0, std::min(iy, g_imgH - 1));
}

void UpdateHotspotEdits()
{
    SetWindowTextW(g_hwndEditX, std::to_wstring(g_hotX).c_str());
    SetWindowTextW(g_hwndEditY, std::to_wstring(g_hotY).c_str());
}

void ReadHotspotFromEdits()
{
    WCHAR buf[16] = {};
    GetWindowTextW(g_hwndEditX, buf, 16); int x = _wtoi(buf);
    GetWindowTextW(g_hwndEditY, buf, 16); int y = _wtoi(buf);
    if (g_bmp) {
        x = std::max(0, std::min(x, g_imgW - 1));
        y = std::max(0, std::min(y, g_imgH - 1));
    }
    g_hotX = x; g_hotY = y;
    UpdateHotspotEdits();
}

void UpdateStatus()
{
    if (!g_hwndStatus) return;
    WCHAR buf[256] = {};
    if (g_bmp) {
        swprintf_s(buf, 256,
            L"  %s  |  Size: %d * %d px  |  Hotspot: (%d, %d)  |  Zoom: %.0f%%  "
            L"  [Click image to set hotspot, Scroll to zoom]",
            g_filePath.c_str(), g_imgW, g_imgH,
            g_hotX, g_hotY, g_zoom * 100.f);
    } else {
        wcscpy_s(buf, L"  Open a PNG file to begin  (File menu or Open button)");
    }
    SetWindowTextW(g_hwndStatus, buf);
}

void LayoutControls(int w, int h)
{
    // Status bar resizes itself
    SendMessage(g_hwndStatus, WM_SIZE, 0, MAKELPARAM(w, h));
    RECT sr{}; GetWindowRect(g_hwndStatus, &sr);
    int sh = sr.bottom - sr.top;

    int canvasH = h - sh - TOOLBAR_H;
    if (canvasH < 0) canvasH = 0;

    // Canvas fills top portion
    MoveWindow(g_hwndCanvas, 0, 0, w, canvasH, TRUE);

    // Toolbar controls anchored to the toolbar strip
    int ty = canvasH;
    int tx = 8;
    int bh = 28, bw = 88;

    MoveWindow(g_hwndBtnOpen,  tx, ty + (TOOLBAR_H - bh) / 2, bw,     bh, TRUE); tx += bw + 6;
    MoveWindow(g_hwndBtnSave,  tx, ty + (TOOLBAR_H - bh) / 2, bw,     bh, TRUE); tx += bw + 20;
    // Separator label
    HWND sep = GetDlgItem(g_hwnd, 9999);
    if (sep) MoveWindow(sep, tx, ty + 8, 1, TOOLBAR_H - 16, TRUE);
    tx += 14;
    // "Hotspot X:" label (created as STATIC)
    HWND lx = GetDlgItem(g_hwnd, 1009);
    HWND ly = GetDlgItem(g_hwnd, 1010);
    if (lx) MoveWindow(lx, tx, ty + (TOOLBAR_H - 20) / 2, 68, 20, TRUE); tx += 71;
    MoveWindow(g_hwndEditX, tx, ty + (TOOLBAR_H - 24) / 2, 52, 24, TRUE); tx += 56;
    if (ly) MoveWindow(ly, tx, ty + (TOOLBAR_H - 20) / 2, 18, 20, TRUE); tx += 21;
    MoveWindow(g_hwndEditY, tx, ty + (TOOLBAR_H - 24) / 2, 52, 24, TRUE); tx += 58;
    MoveWindow(g_hwndBtnApply, tx, ty + (TOOLBAR_H - bh) / 2, 60, bh, TRUE); tx += 66;
    MoveWindow(g_hwndBtnCtr,   tx, ty + (TOOLBAR_H - bh) / 2, 78, bh, TRUE);
}

// ─── File operations ─────────────────────────────────────────────────────────
void LoadBitmapFromPath(HWND hwnd, const std::wstring& path)
{
    Bitmap* nb = new Bitmap(path.c_str());
    if (nb->GetLastStatus() != Ok) {
        delete nb;
        MessageBoxW(hwnd, L"Failed to load the PNG file.\n\nMake sure it is a valid PNG image.",
                    L"Load Error", MB_ICONERROR);
        return;
    }
    delete g_bmp;
    g_bmp = nb;
    g_filePath = path;
    g_imgW = (int)g_bmp->GetWidth();
    g_imgH = (int)g_bmp->GetHeight();
    g_hotX = 0; g_hotY = 0;

    FitZoomToCanvas();
    UpdateHotspotEdits();
    UpdateStatus();

    // Suggest a meaningful window title (filename only, not full path)
    size_t slash = g_filePath.find_last_of(L"\\/");
    std::wstring name = (slash != std::wstring::npos)
                      ? g_filePath.substr(slash + 1) : g_filePath;
    std::wstring title = L"PNG to CUR    " + name;
    SetWindowTextW(hwnd, title.c_str());

    InvalidateRect(g_hwndCanvas, nullptr, TRUE);
}

void DoOpenFile(HWND hwnd)
{
    WCHAR path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"PNG Images\0*.png\0All Files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"Open PNG Image";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        LoadBitmapFromPath(hwnd, path);
}

void DoSaveFile(HWND hwnd)
{
    if (!g_bmp) {
        MessageBoxW(hwnd, L"Please open a PNG file first.", L"No Image", MB_ICONWARNING);
        return;
    }
    ReadHotspotFromEdits();
    UpdateStatus();
    InvalidateRect(g_hwndCanvas, nullptr, FALSE);

    // Build default save path
    std::wstring suggest = g_filePath;
    size_t dot = suggest.rfind(L'.');
    if (dot != std::wstring::npos) suggest = suggest.substr(0, dot);
    suggest += L".cur";

    WCHAR path[MAX_PATH] = {};
    wcsncpy_s(path, suggest.c_str(), MAX_PATH - 1);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"Cursor Files\0*.cur\0All Files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"cur";
    ofn.lpstrTitle  = L"Save Cursor As";
    ofn.Flags       = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;

    if (WriteCurFile(path, g_bmp, g_hotX, g_hotY)) {
        std::wstring msg = L"Cursor saved successfully!\n\nHotspot: ("
            + std::to_wstring(g_hotX) + L", " + std::to_wstring(g_hotY) + L")\n\n"
            + L"You can install it via Control Panel > Mouse > Pointers.";
        MessageBoxW(hwnd, msg.c_str(), L"Saved", MB_ICONINFORMATION);
    } else {
        MessageBoxW(hwnd, L"Failed to write the cursor file.\n\nCheck that the path is writable.",
                    L"Save Error", MB_ICONERROR);
    }
}

// ─── Canvas window procedure ─────────────────────────────────────────────────
LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // ── Paint ────────────────────────────────────────────────────────────────
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int cw = rc.right, ch = rc.bottom;

        // ── Double-buffer ────────────────────────────────────────────────────
        HDC     memDC  = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, cw, ch);
        auto    oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        // ── Background ───────────────────────────────────────────────────────
        RECT full{0, 0, cw, ch};
        {
            HBRUSH bg = CreateSolidBrush(RGB(28, 28, 34));
            FillRect(memDC, &full, bg);
            DeleteObject(bg);
        }

        if (g_bmp)
        {
            UpdateOffsets();
            int dw = (int)(g_imgW * g_zoom);
            int dh = (int)(g_imgH * g_zoom);

            // ── Checkerboard (transparency indicator) ────────────────────────
            int ts = std::max(6, std::min((int)(12 * g_zoom), 18));
            for (int ty2 = 0; ty2 < dh; ty2 += ts) {
                for (int tx2 = 0; tx2 < dw; tx2 += ts) {
                    bool even = (tx2 / ts + ty2 / ts) % 2 == 0;
                    HBRUSH hb = CreateSolidBrush(even ? RGB(200,200,200) : RGB(155,155,155));
                    RECT tile{ g_offX + tx2, g_offY + ty2,
                               g_offX + std::min(tx2 + ts, dw),
                               g_offY + std::min(ty2 + ts, dh) };
                    FillRect(memDC, &tile, hb);
                    DeleteObject(hb);
                }
            }

            // ── Image ────────────────────────────────────────────────────────
            {
                Graphics gfx(memDC);
                gfx.SetInterpolationMode(InterpolationModeNearestNeighbor);
                gfx.SetPixelOffsetMode(PixelOffsetModeHalf);
                gfx.DrawImage(g_bmp, g_offX, g_offY, dw, dh);
            }

            // ── Image border ─────────────────────────────────────────────────
            {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 70, 90));
                SelectObject(memDC, pen);
                SelectObject(memDC, GetStockObject(NULL_BRUSH));
                Rectangle(memDC, g_offX - 1, g_offY - 1,
                          g_offX + dw + 1, g_offY + dh + 1);
                DeleteObject(pen);
            }

            // ── Hotspot crosshair ────────────────────────────────────────────
            int hx = g_offX + (int)(g_hotX * g_zoom + 0.5f);
            int hy = g_offY + (int)(g_hotY * g_zoom + 0.5f);
            int arm = 14;

            // Helper lambda: draw a line with a given pen color/width
            auto Line = [&](COLORREF c, int pw, int x1, int y1, int x2, int y2) {
                HPEN p   = CreatePen(PS_SOLID, pw, c);
                HPEN old = (HPEN)SelectObject(memDC, p);
                MoveToEx(memDC, x1, y1, nullptr);
                LineTo  (memDC, x2, y2);
                SelectObject(memDC, old);
                DeleteObject(p);
            };

            // Drop-shadow
            Line(RGB(0,0,0), 3, hx-arm, hy+1, hx+arm+1, hy+1);
            Line(RGB(0,0,0), 3, hx+1, hy-arm, hx+1, hy+arm+1);
            // Bright red crosshair
            Line(RGB(255, 45, 45), 2, hx-arm, hy, hx+arm+1, hy);
            Line(RGB(255, 45, 45), 2, hx, hy-arm, hx, hy+arm+1);
            // White center dot
            {
                HBRUSH dotBr = CreateSolidBrush(RGB(255, 255, 255));
                HPEN   dotPn = CreatePen(PS_SOLID, 1, RGB(200, 0, 0));
                SelectObject(memDC, dotPn);
                SelectObject(memDC, dotBr);
                Ellipse(memDC, hx-3, hy-3, hx+4, hy+4);
                DeleteObject(dotBr);
                DeleteObject(dotPn);
            }

            // ── Pixel coordinate tooltip near cursor ─────────────────────────
            {
                WCHAR label[32];
                swprintf_s(label, L"(%d, %d)", g_hotX, g_hotY);
                SetTextColor(memDC, RGB(255, 255, 200));
                SetBkColor  (memDC, RGB(30, 30, 30));
                SetBkMode   (memDC, OPAQUE);
                int lx2 = hx + arm + 4, ly2 = hy - 16;
                if (lx2 + 70 > cw) lx2 = hx - arm - 72;
                if (ly2 < 0)       ly2 = hy + arm + 2;
                TextOutW(memDC, lx2, ly2, label, (int)wcslen(label));
                SetBkMode(memDC, TRANSPARENT);
            }

            // ── Zoom badge ───────────────────────────────────────────────────
            {
                WCHAR zoomStr[16];
                swprintf_s(zoomStr, L" %.0f%% ", g_zoom * 100.f);
                SetTextColor(memDC, RGB(200, 200, 200));
                SetBkColor  (memDC, RGB(28, 28, 34));
                SetBkMode   (memDC, OPAQUE);
                TextOutW(memDC, 6, 5, zoomStr, (int)wcslen(zoomStr));
                SetBkMode(memDC, TRANSPARENT);
            }
        }
        else
        {
            // ── Welcome message ───────────────────────────────────────────────
            HFONT font = CreateFontW(22, 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(memDC, font);
            SetTextColor(memDC, RGB(90, 90, 100));
            SetBkMode   (memDC, TRANSPARENT);
            DrawTextW(memDC,
                L"Open a PNG file to begin\n\n"
                L"File  >  Open PNG   or   press  Ctrl+O\n\n"
                L"File  >  Open PNG   or   press  Ctrl+O\n\n"
                L"Then click on the image to place the hotspot",
                -1, &full, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            SelectObject(memDC, oldFont);
            DeleteObject(font);
        }

        // Blit to screen
        BitBlt(hdc, 0, 0, cw, ch, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    // ── Mouse input (hotspot placement) ─────────────────────────────────────
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        g_dragging = true;
        // fall through
    case WM_MOUSEMOVE:
        if (g_bmp && (msg == WM_LBUTTONDOWN || (g_dragging && (wParam & MK_LBUTTON))))
        {
            int mx = (short)LOWORD(lParam);
            int my = (short)HIWORD(lParam);
            CanvasToImage(mx, my, g_hotX, g_hotY);
            UpdateHotspotEdits();
            UpdateStatus();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        g_dragging = false;
        ReleaseCapture();
        return 0;

    // ── Zoom with scroll wheel ───────────────────────────────────────────────
    case WM_MOUSEWHEEL:
    {
        if (!g_bmp) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float factor = (delta > 0) ? 1.25f : (1.0f / 1.25f);
        g_zoom = std::max(0.1f, std::min(g_zoom * factor, 32.0f));
        UpdateOffsets();
        UpdateStatus();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SIZE:
        if (g_bmp) { FitZoomToCanvas(); }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Main window procedure ────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // ── Menu bar ─────────────────────────────────────────────────────────
        HMENU hMenu  = CreateMenu();
        HMENU hFile  = CreatePopupMenu();
        HMENU hView  = CreatePopupMenu();
        HMENU hHelp  = CreatePopupMenu();

        AppendMenuW(hFile, MF_STRING,    IDM_FILE_OPEN,   L"&Open PNG...\tCtrl+O");
        AppendMenuW(hFile, MF_STRING,    IDM_FILE_SAVE,   L"&Save as CUR...\tCtrl+S");
        AppendMenuW(hFile, MF_SEPARATOR, 0,               nullptr);
        AppendMenuW(hFile, MF_STRING,    IDM_FILE_EXIT,   L"E&xit\tAlt+F4");

        AppendMenuW(hView, MF_STRING,    IDM_VIEW_FIT,    L"&Fit to Window\tCtrl+0");
        AppendMenuW(hView, MF_STRING,    IDM_VIEW_ACTUAL, L"&Actual Size (1:1)\tCtrl+1");
        AppendMenuW(hView, MF_SEPARATOR, 0,               nullptr);
        AppendMenuW(hView, MF_STRING,    IDM_VIEW_ZOOMIN, L"Zoom &In\tCtrl+=");
        AppendMenuW(hView, MF_STRING,    IDM_VIEW_ZOOMOUT,L"Zoom &Out\tCtrl+-");

        AppendMenuW(hHelp, MF_STRING,    IDM_HELP_ABOUT,  L"&About");

        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFile, L"&File");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hView, L"&View");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hHelp, L"&Help");
        SetMenu(hwnd, hMenu);

        // ── Register & create canvas child window ─────────────────────────────
        WNDCLASSW cc{};
        cc.lpfnWndProc   = CanvasProc;
        cc.hInstance     = g_hInst;
        cc.lpszClassName = L"PNG2CUR_Canvas";
        cc.hCursor       = LoadCursor(nullptr, IDC_CROSS);
        RegisterClassW(&cc);

        g_hwndCanvas = CreateWindowW(L"PNG2CUR_Canvas", nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 100, 100, hwnd, (HMENU)IDC_CANVAS, g_hInst, nullptr);

        // ── Status bar ────────────────────────────────────────────────────────
        g_hwndStatus = CreateWindowW(L"msctls_statusbar32", nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_STATUSBAR, g_hInst, nullptr);

        // ── Toolbar controls ─────────────────────────────────────────────────
        g_hwndBtnOpen  = CreateWindowW(L"BUTTON", L"Open PNG",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 88, 28, hwnd, (HMENU)IDC_BTN_OPEN, g_hInst, nullptr);

        g_hwndBtnSave  = CreateWindowW(L"BUTTON", L"Save CUR",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 88, 28, hwnd, (HMENU)IDC_BTN_SAVE, g_hInst, nullptr);

        // Labels (STATIC)
        CreateWindowW(L"STATIC", L"Hotspot X:",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 68, 20, hwnd, (HMENU)1009, g_hInst, nullptr);

        g_hwndEditX = CreateWindowW(L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER,
            0, 0, 52, 24, hwnd, (HMENU)IDC_EDIT_X, g_hInst, nullptr);

        CreateWindowW(L"STATIC", L"Y:",
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 18, 20, hwnd, (HMENU)1010, g_hInst, nullptr);

        g_hwndEditY = CreateWindowW(L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER,
            0, 0, 52, 24, hwnd, (HMENU)IDC_EDIT_Y, g_hInst, nullptr);

        g_hwndBtnApply = CreateWindowW(L"BUTTON", L"Apply",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 60, 28, hwnd, (HMENU)IDC_BTN_APPLY, g_hInst, nullptr);

        g_hwndBtnCtr = CreateWindowW(L"BUTTON", L"Center",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 78, 28, hwnd, (HMENU)IDC_BTN_CENTER, g_hInst, nullptr);

        // ── Drag & drop ───────────────────────────────────────────────────────
        DragAcceptFiles(hwnd, TRUE);

        UpdateStatus();
        return 0;
    }

    case WM_SIZE:
    {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        LayoutControls(w, h);
        return 0;
    }

    // ── Keyboard shortcuts ───────────────────────────────────────────────────
    case WM_KEYDOWN:
    {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
            switch ((int)wParam) {
            case 'O': DoOpenFile(hwnd);  return 0;
            case 'S': DoSaveFile(hwnd);  return 0;
            case '0': FitZoomToCanvas(); UpdateStatus(); InvalidateRect(g_hwndCanvas, nullptr, FALSE); return 0;
            case '1': g_zoom = 1.f; UpdateOffsets(); UpdateStatus(); InvalidateRect(g_hwndCanvas, nullptr, FALSE); return 0;
            case VK_OEM_PLUS:
            case VK_ADD:
                g_zoom = std::min(g_zoom * 1.5f, 32.f);
                UpdateOffsets(); UpdateStatus(); InvalidateRect(g_hwndCanvas, nullptr, FALSE); return 0;
            case VK_OEM_MINUS:
            case VK_SUBTRACT:
                g_zoom = std::max(g_zoom / 1.5f, 0.1f);
                UpdateOffsets(); UpdateStatus(); InvalidateRect(g_hwndCanvas, nullptr, FALSE); return 0;
            }
        }
        // Arrow keys for fine hotspot adjustment
        if (g_bmp) {
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            int  step  = shift ? 10 : 1;
            switch ((int)wParam) {
            case VK_LEFT:  g_hotX = std::max(0, g_hotX - step); break;
            case VK_RIGHT: g_hotX = std::min(g_imgW - 1, g_hotX + step); break;
            case VK_UP:    g_hotY = std::max(0, g_hotY - step); break;
            case VK_DOWN:  g_hotY = std::min(g_imgH - 1, g_hotY + step); break;
            default: goto skip_arrow;
            }
            UpdateHotspotEdits(); UpdateStatus();
            InvalidateRect(g_hwndCanvas, nullptr, FALSE);
            skip_arrow:;
        }
        return 0;
    }

    // ── Button / menu commands ───────────────────────────────────────────────
    case WM_COMMAND:
    {
        WORD cmd = LOWORD(wParam);
        switch (cmd) {
        case IDM_FILE_OPEN:
        case IDC_BTN_OPEN:   DoOpenFile(hwnd); break;

        case IDM_FILE_SAVE:
        case IDC_BTN_SAVE:   DoSaveFile(hwnd); break;

        case IDM_FILE_EXIT:  DestroyWindow(hwnd); break;

        case IDC_BTN_APPLY:
            ReadHotspotFromEdits();
            UpdateStatus();
            InvalidateRect(g_hwndCanvas, nullptr, FALSE);
            break;

        case IDC_BTN_CENTER:
            if (g_bmp) {
                g_hotX = g_imgW / 2;
                g_hotY = g_imgH / 2;
                UpdateHotspotEdits(); UpdateStatus();
                InvalidateRect(g_hwndCanvas, nullptr, FALSE);
            }
            break;

        case IDM_VIEW_FIT:
            FitZoomToCanvas(); UpdateStatus();
            InvalidateRect(g_hwndCanvas, nullptr, FALSE);
            break;

        case IDM_VIEW_ACTUAL:
            g_zoom = 1.f; UpdateOffsets(); UpdateStatus();
            InvalidateRect(g_hwndCanvas, nullptr, FALSE);
            break;

        case IDM_VIEW_ZOOMIN:
            g_zoom = std::min(g_zoom * 1.5f, 32.f);
            UpdateOffsets(); UpdateStatus();
            InvalidateRect(g_hwndCanvas, nullptr, FALSE);
            break;

        case IDM_VIEW_ZOOMOUT:
            g_zoom = std::max(g_zoom / 1.5f, 0.1f);
            UpdateOffsets(); UpdateStatus();
            InvalidateRect(g_hwndCanvas, nullptr, FALSE);
            break;

        case IDM_HELP_ABOUT:
            MessageBoxW(hwnd,
                L"PNG to CUR Converter  v1.0\n\n"
                L"Convert any PNG image to a Windows cursor (.cur) file\n"
                L"with a custom hotspot.\n\n"
                L"HOTSPOT EDITING\n"
                L"  - Click / drag on the image to place the crosshair\n"
                L"  - Type exact pixel coords and press Apply\n"
                L"  - Arrow keys (+-1px) or Shift+Arrow (+-10px) for fine tuning\n"
                L"  - Center button sets hotspot to image centre\n\n"
                L"ZOOM\n"
                L"  - Scroll wheel    Ctrl+= / Ctrl+-\n"
                L"  - Ctrl+0 = fit    Ctrl+1 = actual size\n\n"
                L"SUPPORTED SIZES\n"
                L"  Any size is accepted; Windows uses 32*32 most of the time.\n"
                L"  Standard animated cursors also accept 48*48 and 64*64.\n\n"
                L"The hotspot is the pixel Windows uses as the click point.",
                L"About PNG to CUR",
                MB_ICONINFORMATION);
            break;
        }
        // Handle Enter key in edit boxes to apply hotspot
        if (HIWORD(wParam) == EN_CHANGE) {
            // live update handled by Apply button or Enter
        }
        return 0;
    }

    // ── Drag & drop support ──────────────────────────────────────────────────
    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        WCHAR path[MAX_PATH] = {};
        if (DragQueryFileW(hDrop, 0, path, MAX_PATH))
            LoadBitmapFromPath(hwnd, path);
        DragFinish(hDrop);
        return 0;
    }

    // ── Toolbar background (optional cosmetic) ───────────────────────────────
    case WM_CTLCOLORSTATIC:
    {
        HDC hdcStatic = (HDC)wParam;
        SetBkColor(hdcStatic, GetSysColor(COLOR_3DFACE));
        return (LRESULT)GetSysColorBrush(COLOR_3DFACE);
    }

    case WM_GETMINMAXINFO:
    {
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize = { 560, 420 };
        return 0;
    }

    case WM_DESTROY:
        delete g_bmp;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Entry point ─────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmd, int nShow)
{
    g_hInst = hInst;

    // GDI+
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdipToken, &gsi, nullptr);

    // Common controls (status bar etc.)
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // Register main window class
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"PNG2CUR_MainWindow";
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        L"PNG2CUR_MainWindow",
        L"PNG to CUR Converter",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 620,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    // Handle optional command-line PNG argument:  png2cur.exe myimage.png
    if (lpCmd && lpCmd[0] != L'\0') {
        std::wstring arg = lpCmd;
        // Strip surrounding quotes
        if (!arg.empty() && arg.front() == L'"') arg = arg.substr(1);
        if (!arg.empty() && arg.back()  == L'"') arg.pop_back();
        if (!arg.empty()) LoadBitmapFromPath(g_hwnd, arg);
    }

    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_hwnd, &m)) {   // lets Enter/Tab work in edit boxes
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    GdiplusShutdown(g_gdipToken);
    return (int)m.wParam;
}