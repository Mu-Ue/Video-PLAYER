#include "ui/popup_menu.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwchar>
#include <iterator>

namespace {

// The menu bar's colors (see DrawBar in overlay.cpp).
constexpr COLORREF kBack = RGB(0x20, 0x20, 0x20);
constexpr COLORREF kHot = RGB(0x3a, 0x3a, 0x3a);
constexpr COLORREF kText = RGB(0xff, 0xff, 0xff);
constexpr COLORREF kTextDim = RGB(0x70, 0x70, 0x70);
constexpr wchar_t kCheck[] = L"\xE73E";  // CheckMark in Segoe MDL2 Assets, on Windows 10 and 11

// The popup that is showing.
struct {
    HMENU menu = nullptr;
    const MenuItem* items = nullptr;
    int count = 0;
    HFONT font = nullptr, icons = nullptr;
    int textH = 0, itemH = 0, separatorH = 0, inset = 0, radius = 0;
    int checkW = 0, labelW = 0, gap = 0, shortcutW = 0, padRight = 0;  // the columns, left to right
} g;

WNDPROC menuProc = nullptr;  // the menu window class's own procedure

void FillSolid(HDC dc, const RECT& r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, HBRUSH(GetStockObject(DC_BRUSH)));
}

// ExtTextOut rather than DrawText, which would load the complex-script shaper.
void Text(HDC dc, int x, int y, const wchar_t* text) {
    ExtTextOutW(dc, x, y, 0, nullptr, text, UINT(wcslen(text)), nullptr);
}

int TextWidth(HDC dc, const wchar_t* text) {
    SIZE size{};
    GetTextExtentPoint32W(dc, text, int(wcslen(text)), &size);
    return size.cx;
}

void DrawItem(const DRAWITEMSTRUCT& d) {
    const HDC dc = d.hDC;
    const RECT& r = d.rcItem;
    const int saved = SaveDC(dc);
    FillSolid(dc, r, kBack);
    const MenuItem& item = *reinterpret_cast<const MenuItem*>(d.itemData);
    if (!item.cmd) {
        const int y = (r.top + r.bottom) / 2;
        FillSolid(dc, {r.left + g.inset, y, r.right - g.inset, y + 1}, kHot);
    } else {
        if (d.itemState & ODS_SELECTED) {
            SetDCBrushColor(dc, kHot);
            SetDCPenColor(dc, kHot);
            SelectObject(dc, GetStockObject(DC_BRUSH));
            SelectObject(dc, GetStockObject(DC_PEN));
            RoundRect(dc, r.left + g.inset, r.top, r.right - g.inset, r.bottom, g.radius, g.radius);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, d.itemState & (ODS_GRAYED | ODS_DISABLED) ? kTextDim : kText);
        if (item.checked) {
            SelectObject(dc, g.icons);
            SIZE size{};
            GetTextExtentPoint32W(dc, kCheck, 1, &size);
            Text(dc, r.left + (g.checkW - size.cx) / 2, (r.top + r.bottom - size.cy) / 2, kCheck);
        }
        SelectObject(dc, g.font);
        const int x = r.left + g.checkW, y = (r.top + r.bottom - g.textH) / 2;
        Text(dc, x, y, item.label);
        Text(dc, x + g.labelW + g.gap, y, item.shortcut);
    }
    RestoreDC(dc, saved);
}

// Rounds the popup and gives it a gray DWM border (Windows 11), and paints the classic 3D frame that owner-drawn
// menus get in the background color, so only that border shows around it.
LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        const COLORREF border = kHot;
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    } else if (msg == WM_NCPAINT) {
        RECT r;
        GetWindowRect(hwnd, &r);
        OffsetRect(&r, -r.left, -r.top);
        const HDC dc = GetWindowDC(hwnd);
        FillSolid(dc, r, kBack);
        ReleaseDC(hwnd, dc);
        return 0;
    }
    return CallWindowProcW(menuProc, hwnd, msg, wp, lp);
}

// Catches the popup's window as it is created, to subclass it.
LRESULT CALLBACK CbtHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        const HWND hwnd = HWND(wp);
        wchar_t name[8];
        if (GetClassNameW(hwnd, name, int(std::size(name))) && !wcscmp(name, L"#32768")) {
            menuProc = WNDPROC(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, LONG_PTR(MenuProc)));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

}  // namespace

UINT ShowPopupMenu(HWND owner, const RECT& anchor, const MenuItem* items, int count) {
    const UINT dpi = GetDpiForWindow(owner);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) return 0;
    LOGFONTW icons{};
    icons.lfHeight = ncm.lfMenuFont.lfHeight;
    icons.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(icons.lfFaceName, L"Segoe MDL2 Assets");
    g.font = CreateFontIndirectW(&ncm.lfMenuFont);
    g.icons = CreateFontIndirectW(&icons);
    const HBRUSH back = CreateSolidBrush(kBack);
    g.menu = CreatePopupMenu();
    UINT cmd = 0;
    if (g.font && g.icons && back && g.menu) {
        const HDC dc = GetDC(nullptr);
        const HGDIOBJ old = SelectObject(dc, g.font);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        g.textH = tm.tmHeight;
        g.labelW = g.shortcutW = 0;
        for (int i = 0; i < count; ++i) {
            if (!items[i].cmd) continue;
            g.labelW = std::max(g.labelW, TextWidth(dc, items[i].label));
            g.shortcutW = std::max(g.shortcutW, TextWidth(dc, items[i].shortcut));
        }
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);

        g.items = items;
        g.count = count;
        g.itemH = g.textH + MulDiv(12, dpi, 96);
        g.separatorH = MulDiv(9, dpi, 96);
        g.inset = MulDiv(4, dpi, 96);
        g.radius = MulDiv(8, dpi, 96);
        g.checkW = g.inset + MulDiv(28, dpi, 96);
        g.gap = g.shortcutW ? MulDiv(40, dpi, 96) : 0;
        g.padRight = g.inset + MulDiv(16, dpi, 96);
        for (int i = 0; i < count; ++i) {
            const UINT flags = MF_OWNERDRAW | (!items[i].cmd ? MF_SEPARATOR : items[i].enabled ? 0 : MF_GRAYED);
            AppendMenuW(g.menu, flags, items[i].cmd, reinterpret_cast<const wchar_t*>(&items[i]));
        }
        // The check column is drawn with the items, so drop the system's.
        MENUINFO mi{sizeof(mi), MIM_STYLE | MIM_BACKGROUND, MNS_NOCHECK};
        mi.hbrBack = back;
        SetMenuInfo(g.menu, &mi);

        TPMPARAMS params{sizeof(params), anchor};
        const HHOOK hook = SetWindowsHookExW(WH_CBT, CbtHook, nullptr, GetCurrentThreadId());
        cmd = UINT(TrackPopupMenuEx(g.menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL, anchor.left,
                                    anchor.bottom, owner, &params));
        if (hook) UnhookWindowsHookEx(hook);
    }
    if (g.menu) DestroyMenu(g.menu);
    if (back) DeleteObject(back);
    if (g.icons) DeleteObject(g.icons);
    if (g.font) DeleteObject(g.font);
    g = {};
    return cmd;
}

bool HandlePopupMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (!g.menu) return false;
    switch (msg) {
        case WM_MEASUREITEM: {
            auto& mi = *reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mi.CtlType != ODT_MENU) return false;
            const MenuItem& item = *reinterpret_cast<const MenuItem*>(mi.itemData);
            mi.itemWidth = UINT(g.checkW + g.labelW + g.gap + g.shortcutW + g.padRight);
            mi.itemHeight = UINT(item.cmd ? g.itemH : g.separatorH);
            result = TRUE;
            return true;
        }
        case WM_DRAWITEM: {
            const auto& d = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (d.CtlType != ODT_MENU) return false;
            DrawItem(d);
            result = TRUE;
            return true;
        }
        case WM_MENUCHAR: {  // owner-drawn items have no "&" mnemonics, so match the letter here
            if (HMENU(lp) != g.menu) return false;
            wchar_t key = LOWORD(wp);
            CharUpperBuffW(&key, 1);
            result = MAKELRESULT(0, MNC_IGNORE);
            for (int i = 0; i < g.count; ++i) {
                if (g.items[i].cmd && g.items[i].enabled && g.items[i].key == key) {
                    result = MAKELRESULT(i, MNC_EXECUTE);
                    break;
                }
            }
            return true;
        }
    }
    return false;
}
