// gui.c — 标签式图形面板 (借鉴 Pavise 页面结构: 概览/优化/CPU/抑制/体检/还原/日志)
#include "app.h"
#include <windowsx.h>
#include <commdlg.h>

// ------------------------------------------------------------------ 控件 ID
enum {
    ID_TAB = 300,
    // 概览
    ID_BTN_GUARD = 301, ID_RB_SMART, ID_RB_COMP, ID_CB_INTENSITY, ID_CB_TIMERS,
    ID_GRAPH, ID_CARDS, ID_COREGRID,
    // 系统优化
    ID_LV_TWEAKS = 310, ID_BTN_APPLY_SEL, ID_BTN_APPLY_ALL, ID_BTN_REVERT_PERSIST,
    ID_BTN_POWER, ID_BTN_FSO, ID_BTN_HAGS, ID_BTN_CFG,
    // CPU 与游戏
    ID_CB_POLICY = 320, ID_BTN_TUNE_NOW, ID_BTN_WSLOCK,
    // 后台抑制
    ID_LV_SUP = 330, ID_LB_WHITE, ID_ED_WHITE, ID_BTN_WHITE_ADD, ID_BTN_WHITE_DEL,
    // 体检
    ID_LV_CHECK = 340, ID_BTN_RUN_CHECK, ID_BTN_FIX_ALL,
    // 还原中心
    ID_LV_JOURNAL = 350, ID_BTN_RS_SESSION, ID_BTN_RS_PERSIST, ID_BTN_RS_ALL, ID_BTN_J_CLEAR,
    // 日志
    ID_ED_LOG_ACTION = 360, ID_ED_LOG_DIAG, ID_BTN_EXPORT_A, ID_BTN_EXPORT_D, ID_BTN_LOG_CLEAR,
    ID_STATUS = 390
};

static const wchar_t* kPages[7] = {
    L"概览", L"系统优化", L"CPU 与游戏", L"后台抑制", L"体检", L"还原中心", L"日志"
};

// 电竞主题配色
#define COL_BG_DARK     RGB(10, 14, 20)
#define COL_BG_PANEL    RGB(18, 24, 32)
#define COL_BG_CARD     RGB(25, 32, 42)
#define COL_ACCENT      RGB(0, 229, 255)
#define COL_ACCENT2     RGB(255, 107, 53)
#define COL_SUCCESS     RGB(76, 255, 140)
#define COL_WARN        RGB(255, 190, 50)
#define COL_DANGER      RGB(255, 82, 82)
#define COL_TEXT        RGB(220, 228, 240)
#define COL_TEXT_DIM    RGB(140, 152, 170)
#define COL_GLOW        RGB(0, 180, 220)

static HWND  g_tab, g_page[7], g_status;
static int   g_curPage;
static int   g_dpi = 96;
static HFONT g_fUi, g_fMono, g_fBig, g_fTitle, g_fSmall;
static HBRUSH g_brushDark, g_brushCard, g_brushPage, g_brushPanel;

static int Sc(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------- 实时数据
static double g_cpuHist[180];
static int    g_cpuHead;
static double g_cpuNow;
static ULONGLONG g_idleLast, g_kernLast, g_userLast;
static bool   g_cpuPrimed;

static void SampleCpu(void)
{
    FILETIME idle, kern, user;
    if (!GetSystemTimes(&idle, &kern, &user)) return;
    ULONGLONG i = ((ULONGLONG)idle.dwHighDateTime << 32) | idle.dwLowDateTime;
    ULONGLONG k = ((ULONGLONG)kern.dwHighDateTime << 32) | kern.dwLowDateTime;
    ULONGLONG u = ((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime;
    if (g_cpuPrimed) {
        ULONGLONG di = i - g_idleLast, dk = k - g_kernLast, du = u - g_userLast;
        ULONGLONG tot = di + dk + du;
        g_cpuNow = tot ? (double)(dk + du) * 100.0 / (double)tot : 0.0;
        g_cpuHist[g_cpuHead] = g_cpuNow;
        g_cpuHead = (g_cpuHead + 1) % 180;
    }
    g_idleLast = i; g_kernLast = k; g_userLast = u;
    g_cpuPrimed = true;
}

// ---------------------------------------------------------------- 画刷/字体
static void GuiInitGfx_(void);
void GuiInitGfx(void) { GuiInitGfx_(); }
static void GuiInitGfx_(void)
{
    g_brushDark = CreateSolidBrush(COL_BG_DARK);
    g_brushCard = CreateSolidBrush(COL_BG_CARD);
    g_brushPanel= CreateSolidBrush(COL_BG_PANEL);
    g_brushPage = (HBRUSH)(COLOR_BTNFACE + 1);
    g_fUi  = CreateFontW(-Sc(15), 0,0,0, FW_NORMAL,0,0,0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_SWISS, L"Microsoft YaHei UI");
    g_fMono= CreateFontW(-Sc(14), 0,0,0, FW_NORMAL,0,0,0, FIXED_PITCH|FF_MODERN,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH|FF_MODERN, L"Consolas");
    g_fBig = CreateFontW(-Sc(34), 0,0,0, FW_BOLD,0,0,0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_SWISS, L"Segoe UI");
    g_fTitle = CreateFontW(-Sc(20), 0,0,0, FW_BOLD,0,0,0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_SWISS, L"Microsoft YaHei UI");
    g_fSmall = CreateFontW(-Sc(11), 0,0,0, FW_NORMAL,0,0,0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_SWISS, L"Microsoft YaHei UI");
}

// 状态灯
static void DrawStatusDot(HDC dc, int x, int y, bool on)
{
    HBRUSH br = CreateSolidBrush(on ? COL_SUCCESS : COL_DANGER);
    HBRUSH old = (HBRUSH)SelectObject(dc, br);
    Ellipse(dc, x, y, x + 10, y + 10);
    SelectObject(dc, old);
    DeleteObject(br);
    HPEN pen = CreatePen(PS_SOLID, 1, on ? COL_SUCCESS : COL_DANGER);
    HPEN op = (HPEN)SelectObject(dc, pen);
    Ellipse(dc, x - 2, y - 2, x + 12, y + 12);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// ---------------------------------------------------------------- 曲线控件
static LRESULT CALLBACK GraphProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        // 渐变背景
        HPEN penGrad = CreatePen(PS_SOLID, 1, COL_BG_DARK);
        HPEN oldG = (HPEN)SelectObject(dc, penGrad);
        for (int y = rc.top; y < rc.bottom; y++) {
            int hgt = rc.bottom - rc.top; if (hgt < 1) hgt = 1;
            int r = 10 + (18 - 10) * (y - rc.top) / hgt;
            int g = 14 + (24 - 14) * (y - rc.top) / hgt;
            int b = 20 + (32 - 20) * (y - rc.top) / hgt;
            DeleteObject(penGrad);
            penGrad = CreatePen(PS_SOLID, 1, RGB(r, g, b));
            SelectObject(dc, penGrad);
            MoveToEx(dc, rc.left, y, NULL); LineTo(dc, rc.right, y);
        }
        SelectObject(dc, oldG); DeleteObject(penGrad);
        // 网格线
        HPEN penGrid = CreatePen(PS_SOLID, 1, RGB(30, 40, 55));
        HPEN old = (HPEN)SelectObject(dc, penGrid);
        for (int gy = 1; gy < 4; gy++) {
            int y = rc.top + (rc.bottom - rc.top) * gy / 4;
            MoveToEx(dc, rc.left, y, NULL); LineTo(dc, rc.right, y);
        }
        for (int gx = 1; gx < 10; gx++) {
            int x = rc.left + (rc.right - rc.left) * gx / 10;
            MoveToEx(dc, x, rc.top, NULL); LineTo(dc, x, rc.bottom);
        }
        SelectObject(dc, old); DeleteObject(penGrid);

        int wpx = rc.right - rc.left, hpx = rc.bottom - rc.top;
        // 底层光晕
        HPEN penGlow = CreatePen(PS_SOLID, 6, COL_GLOW);
        old = (HPEN)SelectObject(dc, penGlow);
        SetROP2(dc, R2_MERGEPEN);
        int prevX = -1, prevY = -1;
        for (int i = 0; i < 180; i++) {
            int idx = (g_cpuHead + i) % 180;
            double v = g_cpuHist[idx];
            int x = rc.left + wpx * i / 179;
            int y = rc.bottom - 4 - (int)((hpx - 8) * v / 100.0);
            if (prevX >= 0 && v > 0) { MoveToEx(dc, prevX, prevY, NULL); LineTo(dc, x, y); }
            prevX = x; prevY = y;
        }
        SelectObject(dc, old); DeleteObject(penGlow);
        // 主曲线
        HPEN penLine = CreatePen(PS_SOLID, 2, COL_ACCENT);
        old = (HPEN)SelectObject(dc, penLine);
        prevX = -1;
        for (int i = 0; i < 180; i++) {
            int idx = (g_cpuHead + i) % 180;
            double v = g_cpuHist[idx];
            int x = rc.left + wpx * i / 179;
            int y = rc.bottom - 4 - (int)((hpx - 8) * v / 100.0);
            if (prevX >= 0 && v > 0) { MoveToEx(dc, prevX, prevY, NULL); LineTo(dc, x, y); }
            prevX = x; prevY = y;
        }
        SelectObject(dc, old); DeleteObject(penLine);

        // 数值显示
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, COL_ACCENT);
        HFONT of = (HFONT)SelectObject(dc, g_fBig);
        wchar_t t[64];
        swprintf(t, 64, L"%.0f%%", g_cpuNow);
        TextOutW(dc, rc.left + 16, rc.top + 8, t, (int)wcslen(t));
        SelectObject(dc, g_fSmall);
        SetTextColor(dc, COL_TEXT_DIM);
        TextOutW(dc, rc.left + 16, rc.top + 52, L"CPU 占用", 8);
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- 卡片条
static LRESULT CALLBACK CardsProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, g_brushDark);
        SetBkMode(dc, TRANSPARENT);

        LONG pid = InterlockedCompareExchange(&g_gamePid, 0, 0);
        LONG guard = InterlockedCompareExchange(&g_guardActive, 0, 0);
        wchar_t s1[16], s2[16], s3[16];
        if (guard == 0)        wcscpy(s1, L"离线");
        else if (pid)          wcscpy(s1, L"游戏中");
        else                   wcscpy(s1, L"待命");
        swprintf(s2, 16, L"%d", InterlockedCompareExchange(&g_suppressedCount, 0, 0));
        swprintf(s3, 16, L"%d", InterlockedCompareExchange(&g_restoredCount, 0, 0));

        struct Card { const wchar_t* title; const wchar_t* val; COLORREF accent; bool dot; };
        struct Card cards[4] = {
            { L"会话状态", s1, guard ? (pid ? COL_ACCENT : COL_WARN) : COL_DANGER, true },
            { L"已抑制后台", s2, COL_ACCENT2, false },
            { L"已还原项", s3, COL_SUCCESS, false },
            { L"权限", IsRunAsAdmin() ? L"管理员" : L"普通",
              IsRunAsAdmin() ? COL_SUCCESS : COL_WARN, true },
        };
        int cw = (rc.right - rc.left) / 4;
        HFONT of = (HFONT)SelectObject(dc, g_fUi);
        for (int i = 0; i < 4; i++) {
            RECT c = { rc.left + i*cw + 6, rc.top + 8,
                       rc.left + (i+1)*cw - 6, rc.bottom - 8 };
            // 卡片背景
            HBRUSH br = CreateSolidBrush(COL_BG_CARD);
            HBRUSH ob = (HBRUSH)SelectObject(dc, br);
            RoundRect(dc, c.left, c.top, c.right, c.bottom, 8, 8);
            SelectObject(dc, ob); DeleteObject(br);
            // 左侧强调线
            HPEN pen = CreatePen(PS_SOLID, 3, cards[i].accent);
            HPEN op = (HPEN)SelectObject(dc, pen);
            MoveToEx(dc, c.left + 2, c.top + 4, NULL);
            LineTo(dc, c.left + 2, c.bottom - 4);
            SelectObject(dc, op); DeleteObject(pen);
            // 标题
            RECT rTitle = { c.left + 12, c.top + 8, c.right - 8, c.top + 26 };
            SetTextColor(dc, COL_TEXT_DIM);
            HFONT os = (HFONT)SelectObject(dc, g_fSmall);
            DrawTextW(dc, cards[i].title, -1, &rTitle, DT_LEFT | DT_SINGLELINE);
            SelectObject(dc, os);
            // 状态灯
            if (cards[i].dot) {
                bool on = (i == 0 && guard) || (i == 3 && IsRunAsAdmin());
                DrawStatusDot(dc, c.left + 12, c.top + 30, on);
            }
            // 数值
            SetTextColor(dc, cards[i].accent);
            int valLen = (int)wcslen(cards[i].val);
            HFONT fVal = (valLen > 4) ? g_fTitle : g_fBig;
            HFONT ob2 = (HFONT)SelectObject(dc, fVal);
            RECT rVal = { c.left + 28, c.top + 24, c.right - 8, c.bottom - 4 };
            DrawTextW(dc, cards[i].val, -1, &rVal, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(dc, ob2);
        }
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- 核网格
static LRESULT CALLBACK CoresProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, g_brushPanel);
        SetBkMode(dc, TRANSPARENT);

        int cnt = g_topo.ok ? g_topo.count : 0;
        if (cnt == 0) {
            SetTextColor(dc, COL_TEXT_DIM);
            HFONT of = (HFONT)SelectObject(dc, g_fTitle);
            TextOutW(dc, 20, 20, L"拓扑不可用", 8);
            SelectObject(dc, of);
            EndPaint(h, &ps); return 0;
        }
        // 当前 CS2 亲和性掩码
        DWORD_PTR curMask = 0;
        LONG pid = InterlockedCompareExchange(&g_gamePid, 0, 0);
        bool haveCs2 = false;
        if (pid) {
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
            if (hp) {
                DWORD_PTR pm, sm;
                if (GetProcessAffinityMask(hp, &pm, &sm)) { curMask = pm; haveCs2 = true; }
                CloseHandle(hp);
            }
        }
        int cols = (cnt > 16) ? 8 : (cnt > 8) ? 8 : 4;
        int rows = (cnt + cols - 1) / cols;
        int cw = (rc.right - rc.left - 24) / cols;
        int ch = (rc.bottom - rc.top - 40) / rows;
        if (ch > 56) ch = 56;
        HFONT of = (HFONT)SelectObject(dc, g_fUi);
        wchar_t lbl[16];
        for (int i = 0; i < cnt; i++) {
            int r = i / cols, c = i % cols;
            RECT b = { rc.left + 12 + c*cw + 3, rc.top + 30 + r*ch + 3,
                       rc.left + 12 + (c+1)*cw - 3, rc.top + 30 + (r+1)*ch - 3 };
            COLORREF fill, border;
            if (haveCs2) {
                fill   = (curMask & ((DWORD_PTR)1 << i)) ? RGB(0, 60, 80) : RGB(50, 25, 25);
                border = (curMask & ((DWORD_PTR)1 << i)) ? COL_ACCENT : COL_DANGER;
            } else {
                bool isP = g_topo.hybrid && g_topo.effClass[i] == g_topo.maxEff;
                fill   = isP ? RGB(25, 50, 80) : RGB(50, 40, 25);
                border = isP ? COL_ACCENT : COL_ACCENT2;
            }
            HBRUSH br = CreateSolidBrush(fill);
            HBRUSH ob = (HBRUSH)SelectObject(dc, br);
            RoundRect(dc, b.left, b.top, b.right, b.bottom, 6, 6);
            SelectObject(dc, ob); DeleteObject(br);
            HPEN pen = CreatePen(PS_SOLID, 2, border);
            HPEN op = (HPEN)SelectObject(dc, pen);
            RoundRect(dc, b.left, b.top, b.right, b.bottom, 6, 6);
            SelectObject(dc, op); DeleteObject(pen);
            SetTextColor(dc, COL_TEXT);
            swprintf(lbl, 16, L"%d%s", i, g_topo.hybrid ? TopoClassLabel(i) : L"");
            RECT rt = b;
            DrawTextW(dc, lbl, -1, &rt, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        // 图例
        SetTextColor(dc, COL_TEXT_DIM);
        HFONT os = (HFONT)SelectObject(dc, g_fSmall);
        const wchar_t* legend = haveCs2 ? L"● 已分配  ○ 未分配" :
                                (g_topo.hybrid ? L"● P核  ○ E核" : L"逻辑核心分布");
        TextOutW(dc, rc.left + 12, rc.top + 4, legend, lstrlenW(legend));
        SelectObject(dc, os);
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- 自绘日志控件
// 不再依赖 EDIT: 视觉样式下只读 EDIT 会忽略 WM_CTLCOLOR 返回的画刷,
// 导致浅色文字压在系统白底上不可见。改为 WM_PAINT 直接从内存环形缓冲绘制。
typedef struct {
    int  q;        // 0=操作日志 1=诊断日志
    int  top;      // 首行索引
    bool follow;   // 贴底跟随
} LogView;

static int LogLineH(HDC dc, HFONT f)
{
    HFONT of = (HFONT)SelectObject(dc, f);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    return tm.tmHeight + 2;
}

static void LogSyncScroll(HWND h, LogView* v, int total, int vis)
{
    SCROLLINFO si; memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = total > 0 ? total - 1 : 0;
    si.nPage = (UINT)(vis > 0 ? vis : 1);
    si.nPos = v->top;
    SetScrollInfo(h, SB_VERT, &si, TRUE);
}

static LRESULT CALLBACK LogProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    LogView* v = (LogView*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        v = (LogView*)calloc(1, sizeof(LogView));
        if (v) { v->q = (int)(INT_PTR)cs->lpCreateParams; v->follow = true; }
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)v);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, g_brushPanel);
        // 顶部装饰线
        HPEN pen = CreatePen(PS_SOLID, 1, COL_ACCENT);
        HPEN op = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, 0, 0, NULL); LineTo(dc, rc.right, 0);
        SelectObject(dc, op); DeleteObject(pen);
        if (!v) { EndPaint(h, &ps); return 0; }

        int lh = LogLineH(dc, g_fMono);
        int vis = lh > 0 ? (rc.bottom - rc.top - 8) / lh : 0;
        int total = LogLineCount(v->q);
        int maxtop = total > vis ? total - vis : 0;
        if (v->follow) v->top = maxtop;
        if (v->top < 0) v->top = 0;
        if (v->top > maxtop) v->top = maxtop;
        LogSyncScroll(h, v, total, vis);

        HFONT of = (HFONT)SelectObject(dc, g_fMono);
        SetBkMode(dc, TRANSPARENT);
        wchar_t line[320];
        for (int i = 0; i < vis; i++) {
            int idx = v->top + i;
            if (idx >= total) break;
            if (!LogGetLine(v->q, idx, line, 320)) break;
            COLORREF c = COL_TEXT;
            if (wcsstr(line, L"[!]")) c = COL_WARN;
            else if (wcsstr(line, L"[-]")) c = COL_DANGER;
            else if (wcsstr(line, L"[+]")) c = COL_SUCCESS;
            else if (wcsstr(line, L"===")) c = COL_ACCENT;
            SetTextColor(dc, c);
            RECT r = { rc.left + 10, rc.top + 4 + i*lh, rc.right - 10, rc.top + 4 + (i+1)*lh };
            DrawTextW(dc, line, -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_MOUSEWHEEL: {
        if (!v) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * -3;
        v->top += delta;
        v->follow = false;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_VSCROLL: {
        if (!v) return 0;
        RECT rc; GetClientRect(h, &rc);
        HDC dc = GetDC(h);
        int lh = LogLineH(dc, g_fMono);
        ReleaseDC(h, dc);
        int vis = lh > 0 ? (rc.bottom - rc.top) / lh : 1;
        int total = LogLineCount(v->q);
        int maxtop = total > vis ? total - vis : 0;
        switch (LOWORD(w)) {
        case SB_LINEUP:   v->top -= 1; break;
        case SB_LINEDOWN: v->top += 1; break;
        case SB_PAGEUP:   v->top -= vis; break;
        case SB_PAGEDOWN: v->top += vis; break;
        case SB_TOP:      v->top = 0; break;
        case SB_BOTTOM:   v->top = maxtop; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si; memset(&si, 0, sizeof(si));
            si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
            GetScrollInfo(h, SB_VERT, &si);
            v->top = si.nTrackPos;
            break;
        }
        default: return 0;
        }
        if (v->top < 0) v->top = 0;
        if (v->top > maxtop) v->top = maxtop;
        v->follow = (v->top >= maxtop);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(h);
        return 0;
    case WM_DESTROY:
        if (v) { SetWindowLongPtrW(h, GWLP_USERDATA, 0); free(v); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- 页面容器
static LRESULT HandleCtlColor(UINT m, WPARAM w, LPARAM l)
{
    (void)m;
    HDC dc = (HDC)w;
    HWND ctl = (HWND)l;
    wchar_t cls[32]; cls[0] = 0;
    GetClassNameW(ctl, cls, 32);
    if (!_wcsicmp(cls, L"EDIT")) {              // 输入框: 深色底 + 浅色字
        SetTextColor(dc, RGB(210,220,230));
        SetBkColor(dc, RGB(24,28,36));
        return (INT_PTR)g_brushDark;
    }
    SetTextColor(dc, COL_TEXT);
    SetBkMode(dc, TRANSPARENT);
    return (INT_PTR)g_brushDark;
}

static LRESULT CALLBACK PageProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return HandleCtlColor(m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND MakePage(HWND parent, int idx)
{
    RECT pr; GetClientRect(parent, &pr);
    int tabH = Sc(30);
    HWND pg = CreateWindowExW(0, L"CS2Page", NULL,
        WS_CHILD | WS_CLIPCHILDREN,
        0, tabH, pr.right, pr.bottom - tabH,
        parent, (HMENU)(INT_PTR)(400 + idx),
        (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    SendMessageW(pg, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    return pg;
}

// ---------------------------------------------------------------- 通用创建
static HWND MkBtn(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h)
{
    HWND b = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        Sc(x), Sc(y), Sc(w), Sc(h), parent, (HMENU)(INT_PTR)id,
        (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    return b;
}

static HWND MkStatic(HWND parent, const wchar_t* text, int x, int y, int w, int h)
{
    HWND s = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL,
        Sc(x), Sc(y), Sc(w), Sc(h), parent, NULL,
        (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    SendMessageW(s, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    return s;
}

static HWND MkLv(HWND parent, int id, const wchar_t* cols[], int ncol,
                 int x, int y, int w, int h, bool checkboxes)
{
    HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        Sc(x), Sc(y), Sc(w), Sc(h), parent, (HMENU)(INT_PTR)id,
        (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    ListView_SetExtendedListViewStyle(lv,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
        (checkboxes ? LVS_EX_CHECKBOXES : 0));
    LVCOLUMNW c; memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (int i = 0; i < ncol; i++) {
        c.pszText = (LPWSTR)cols[i];
        c.iSubItem = i;
        c.cx = (i == 0) ? Sc(w - 60 - 130 * (ncol - 1)) :
             (ncol == 2 ? Sc(130) : Sc(w / ncol));
        if (i == ncol - 1) c.cx = Sc(130);
        ListView_InsertColumn(lv, i, &c);
    }
    SendMessageW(lv, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    return lv;
}

static HWND g_lvTweaks, g_lvSup, g_lvCheck, g_lvJournal, g_lbWhite;
static HWND g_btnGuard, g_cbIntensity, g_cbPolicy, g_cbTimers;
static HWND g_edAction, g_edDiag, g_hGraph, g_hCards, g_hCores;
static Finding g_findings[64];
static int g_findN;

// ---------------------------------------------------------------- 页面构建
static void BuildPageOverview(HWND pg)
{
    g_hCards = CreateWindowExW(0, L"CS2Cards", NULL, WS_CHILD | WS_VISIBLE,
        0, 0, Sc(660), Sc(96), pg, (HMENU)(INT_PTR)ID_CARDS,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), NULL);
    g_hGraph = CreateWindowExW(0, L"CS2Graph", NULL, WS_CHILD | WS_VISIBLE,
        0, Sc(100), Sc(660), Sc(150), pg, (HMENU)(INT_PTR)ID_GRAPH,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), NULL);

    g_btnGuard = MkBtn(pg, L"▶ 启动守护", ID_BTN_GUARD, 12, 262, 160, 40);
    MkStatic(pg, L"模式:", 190, 272, 40, 20);
    CreateWindowExW(0, L"BUTTON", L"智能", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON|WS_GROUP,
        Sc(230), Sc(268), Sc(70), Sc(26), pg, (HMENU)(INT_PTR)ID_RB_SMART,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), NULL);
    CreateWindowExW(0, L"BUTTON", L"竞技", WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON,
        Sc(300), Sc(268), Sc(70), Sc(26), pg, (HMENU)(INT_PTR)ID_RB_COMP,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), NULL);
    SendMessageW(GetDlgItem(pg, g_set.mode == 1 ? ID_RB_COMP : ID_RB_SMART),
                 BM_SETCHECK, BST_CHECKED, 0);
    MkStatic(pg, L"后台治理:", 380, 272, 70, 20);
    g_cbIntensity = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,
        Sc(450), Sc(268), Sc(110), Sc(160), pg, (HMENU)(INT_PTR)ID_CB_INTENSITY,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), NULL);
    SendMessageW(g_cbIntensity, CB_ADDSTRING, 0, (LPARAM)L"关闭");
    SendMessageW(g_cbIntensity, CB_ADDSTRING, 0, (LPARAM)L"普通");
    SendMessageW(g_cbIntensity, CB_ADDSTRING, 0, (LPARAM)L"激进");
    SendMessageW(g_cbIntensity, CB_SETCURSEL, g_set.intensity, 0);

    g_cbTimers = CreateWindowExW(0, L"BUTTON", L"守护时启用 1ms 高精度时钟",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
        Sc(12), Sc(312), Sc(240), Sc(24), pg, (HMENU)(INT_PTR)ID_CB_TIMERS,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), NULL);
    SendMessageW(g_cbTimers, BM_SETCHECK, g_set.timers ? BST_CHECKED : BST_UNCHECKED, 0);
    MkStatic(pg,
        L"守护开启后：检测到 cs2.exe 自动应用会话优化 (优先级/亲和性/EcoQoS/电源)，"
        L"游戏退出自动还原。持久项在「还原中心」管理。",
        270, 306, 390, 60);
}

static void BuildPageTweaks(HWND pg)
{
    const wchar_t* cols[] = { L"优化项", L"状态" };
    g_lvTweaks = MkLv(pg, ID_LV_TWEAKS, cols, 2, 12, 12, 636, 300, true);
    MkBtn(pg, L"应用勾选项", ID_BTN_APPLY_SEL, 12, 322, 130, 34);
    MkBtn(pg, L"一键全部应用", ID_BTN_APPLY_ALL, 150, 322, 130, 34);
    MkBtn(pg, L"还原持久优化", ID_BTN_REVERT_PERSIST, 288, 322, 130, 34);
    MkBtn(pg, L"高性能电源", ID_BTN_POWER, 426, 322, 110, 34);
    MkBtn(pg, L"禁用FSO", ID_BTN_FSO, 544, 322, 104, 34);
    MkBtn(pg, L"HAGS开关", ID_BTN_HAGS, 426, 362, 110, 34);
    MkBtn(pg, L"生成autoexec", ID_BTN_CFG, 544, 362, 104, 34);
    MkStatic(pg, L"每项应用前旧值写入 journal；持久项可在还原中心一键恢复。",
             12, 402, 620, 20);
}

static void BuildPageCpu(HWND pg)
{
    g_hCores = CreateWindowExW(0, L"CS2Cores", NULL, WS_CHILD | WS_VISIBLE,
        0, 0, Sc(660), Sc(150), pg, (HMENU)(INT_PTR)ID_COREGRID,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), NULL);
    MkStatic(pg, L"亲和性策略:", 12, 162, 90, 20);
    g_cbPolicy = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,
        Sc(105), Sc(158), Sc(240), Sc(200), pg, (HMENU)(INT_PTR)ID_CB_POLICY,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), NULL);
    SendMessageW(g_cbPolicy, CB_ADDSTRING, 0, (LPARAM)L"保持默认");
    SendMessageW(g_cbPolicy, CB_ADDSTRING, 0, (LPARAM)L"剔除超线程 (推荐)");
    SendMessageW(g_cbPolicy, CB_ADDSTRING, 0, (LPARAM)L"剔除 E-core (大小核)");
    SendMessageW(g_cbPolicy, CB_ADDSTRING, 0, (LPARAM)L"全部核心");
    SendMessageW(g_cbPolicy, CB_ADDSTRING, 0, (LPARAM)L"剔除逻辑核 0/1 (旧法)");
    SendMessageW(g_cbPolicy, CB_SETCURSEL, g_set.policy, 0);
    MkBtn(pg, L"立即调优 CS2", ID_BTN_TUNE_NOW, 360, 156, 140, 30);
    MkBtn(pg, L"锁定工作集", ID_BTN_WSLOCK, 510, 156, 130, 30);
    MkStatic(pg,
        L"亲和性只收窄到游戏原本允许的范围内；EcoQoS 解除节流 + 定时器精度合并处理。\n"
        L"P/E 分色基于真实 EfficiencyClass 拓扑，非大小核机型自动跳过 E-core 策略。",
        12, 196, 630, 60);
}

static void BuildPageSuppress(HWND pg)
{
    const wchar_t* cols[] = { L"被抑制进程", L"PID" };
    g_lvSup = MkLv(pg, ID_LV_SUP, cols, 2, 12, 12, 636, 170, false);
    MkStatic(pg, L"白名单 / 保护名单 (核心项不可删除):", 12, 192, 300, 20);
    g_lbWhite = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY,
        Sc(12), Sc(214), Sc(420), Sc(150), pg, (HMENU)(INT_PTR)ID_LB_WHITE,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), NULL);
    SendMessageW(g_lbWhite, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
        Sc(440), Sc(214), Sc(208), Sc(26), pg, (HMENU)(INT_PTR)ID_ED_WHITE,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), NULL);
    SendMessageW(ed, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    MkBtn(pg, L"添加", ID_BTN_WHITE_ADD, 440, 246, 100, 30);
    MkBtn(pg, L"删除选中", ID_BTN_WHITE_DEL, 548, 246, 100, 30);
    MkStatic(pg,
        L"治理逻辑：白名单 + 系统关键进程 + 游戏链永不触碰；普通档降优先级，"
        L"激进档附加 EcoQoS 效率模式。Smart 模式仅游戏前台时治理。",
        12, 372, 630, 40);
}

static void BuildPageCheckup(HWND pg)
{
    const wchar_t* cols[] = { L"体检结论", L"级别" };
    g_lvCheck = MkLv(pg, ID_LV_CHECK, cols, 2, 12, 12, 636, 320, false);
    MkBtn(pg, L"重新体检", ID_BTN_RUN_CHECK, 12, 342, 130, 34);
    MkBtn(pg, L"一键修复全部", ID_BTN_FIX_ALL, 150, 342, 130, 34);
    MkStatic(pg, L"绿=已优化 橙=需修复 蓝=提示 灰=未知/权限不足", 290, 350, 360, 20);
}

static void BuildPageRestore(HWND pg)
{
    const wchar_t* cols[] = { L"还原项", L"类型" };
    g_lvJournal = MkLv(pg, ID_LV_JOURNAL, cols, 2, 12, 12, 636, 300, false);
    MkBtn(pg, L"还原会话项", ID_BTN_RS_SESSION, 12, 322, 130, 34);
    MkBtn(pg, L"还原持久项", ID_BTN_RS_PERSIST, 150, 322, 130, 34);
    MkBtn(pg, L"全部还原", ID_BTN_RS_ALL, 288, 322, 130, 34);
    MkBtn(pg, L"清空记录(危险)", ID_BTN_J_CLEAR, 426, 322, 150, 34);
    MkStatic(pg,
        L"journal 实时落盘：异常退出/断电后，下次启动自动恢复未完成还原。",
        12, 366, 620, 20);
}

static void BuildPageLog(HWND pg)
{
    g_edAction = CreateWindowExW(WS_EX_CLIENTEDGE, L"CS2Log", NULL,
        WS_CHILD|WS_VISIBLE|WS_VSCROLL,
        Sc(12), Sc(34), Sc(636), Sc(150), pg, (HMENU)(INT_PTR)ID_ED_LOG_ACTION,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), (LPVOID)(INT_PTR)0);
    g_edDiag = CreateWindowExW(WS_EX_CLIENTEDGE, L"CS2Log", NULL,
        WS_CHILD|WS_VISIBLE|WS_VSCROLL,
        Sc(12), Sc(224), Sc(636), Sc(150), pg, (HMENU)(INT_PTR)ID_ED_LOG_DIAG,
        (HINSTANCE)GetWindowLongPtrW(pg, GWLP_HINSTANCE), (LPVOID)(INT_PTR)1);
    MkStatic(pg, L"操作日志 (session log)", 12, 10, 300, 20);
    MkStatic(pg, L"诊断日志 (diagnostic)", 12, 200, 300, 20);
    MkBtn(pg, L"导出操作", ID_BTN_EXPORT_A, 400, 10, 110, 26);
    MkBtn(pg, L"导出诊断", ID_BTN_EXPORT_D, 520, 10, 110, 26);
    MkBtn(pg, L"清空显示", ID_BTN_LOG_CLEAR, 400, 200, 110, 26);
}

// ---------------------------------------------------------------- 刷新
void GuiRefreshTweaks(void)
{
    if (!g_lvTweaks) return;
    ListView_DeleteAllItems(g_lvTweaks);
    int n; const RegTweak* t = TweakTable(&n);
    for (int i = 0; i < n; i++) {
        LVITEMW it; memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = i;
        it.pszText = (LPWSTR)t[i].label;
        ListView_InsertItem(g_lvTweaks, &it);
        bool applied = TweakIsApplied(&t[i]);
        const wchar_t* st = applied ? L"已优化" :
            (t[i].needsAdmin && !IsRunAsAdmin() ? L"需管理员" : L"未优化");
        ListView_SetItemText(g_lvTweaks, i, 1, (LPWSTR)st);
        ListView_SetCheckState(g_lvTweaks, i, applied);   // 勾选 = 已启用
    }
}

void GuiRefreshSuppressList(void)
{
    if (!g_lvSup) return;
    ListView_DeleteAllItems(g_lvSup);
    DWORD pids[64]; wchar_t names[64][32];
    int cnt = SuppressSnapshot(pids, names, 64);
    for (int i = 0; i < cnt; i++) {
        LVITEMW it; memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = i; it.pszText = names[i];
        ListView_InsertItem(g_lvSup, &it);
        wchar_t pidTxt[16]; swprintf(pidTxt, 16, L"%lu", pids[i]);
        ListView_SetItemText(g_lvSup, i, 1, pidTxt);
    }
}

void GuiRefreshWhitelist(void)
{
    if (!g_lbWhite) return;
    SendMessageW(g_lbWhite, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < WhitelistCount(); i++)
        SendMessageW(g_lbWhite, LB_ADDSTRING, 0, (LPARAM)WhitelistGet(i));
}

void GuiRefreshJournal(void)
{
    if (!g_lvJournal) return;
    ListView_DeleteAllItems(g_lvJournal);
    for (int i = 0; i < g_journalCount; i++) {
        JournalEntry* j = &g_journal[i];
        if (!j->applied) continue;
        LVITEMW it; memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = i; it.pszText = j->name;
        ListView_InsertItem(g_lvJournal, &it);
        const wchar_t* kind = j->persistent ? L"持久" : L"会话";
        ListView_SetItemText(g_lvJournal, i, 1, (LPWSTR)kind);
    }
}

void GuiRefreshCheckup(void)
{
    if (!g_lvCheck) return;
    ListView_DeleteAllItems(g_lvCheck);
    for (int i = 0; i < g_findN; i++) {
        LVITEMW it; memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = i;
        it.pszText = g_findings[i].text; it.lParam = g_findings[i].level;
        ListView_InsertItem(g_lvCheck, &it);
        const wchar_t* lvl = g_findings[i].level == 0 ? L"已优化" :
                             g_findings[i].level == 2 ? L"需修复" :
                             g_findings[i].level == 1 ? L"提示" : L"未知";
        ListView_SetItemText(g_lvCheck, i, 1, (LPWSTR)lvl);
    }
}

void GuiRefreshCpuGrid(void)
{
    if (g_hCores) InvalidateRect(g_hCores, NULL, FALSE);
    if (g_hCards) InvalidateRect(g_hCards, NULL, FALSE);
}

// ---------------------------------------------------------------- 体检着色
static LRESULT CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);

void GuiOnTick(void)
{
    SampleCpu();
    if (g_hGraph) InvalidateRect(g_hGraph, NULL, FALSE);
    if (g_curPage == 0) GuiRefreshCpuGrid();
    if (g_curPage == 3 && InterlockedCompareExchange(&g_guardActive, 0, 0))
        GuiRefreshSuppressList();
    if (g_curPage == 6) {
        GuiLogDrain(NULL, NULL);
        if (g_edAction) InvalidateRect(g_edAction, NULL, FALSE);
        if (g_edDiag) InvalidateRect(g_edDiag, NULL, FALSE);
    }

    LONG pid = InterlockedCompareExchange(&g_gamePid, 0, 0);
    LONG guard = InterlockedCompareExchange(&g_guardActive, 0, 0);
    wchar_t t[160];
    if (g_status) {
        if (guard && pid)
            swprintf(t, 160, L"%s | 守护中 | CS2 PID: %ld | 抑制 %d | 还原 %d | %s",
                     APP_VERSION, pid, (int)InterlockedCompareExchange(&g_suppressedCount,0,0),
                     (int)InterlockedCompareExchange(&g_restoredCount,0,0),
                     IsRunAsAdmin() ? L"管理员" : L"普通权限");
        else if (guard)
            swprintf(t, 160, L"%s | 守护待命 | 等待 cs2.exe | %s",
                     APP_VERSION, IsRunAsAdmin() ? L"管理员" : L"普通权限");
        else
            swprintf(t, 160, L"%s | 守护关闭 | %s | CPU %.0f%%",
                     APP_VERSION, IsRunAsAdmin() ? L"管理员" : L"普通权限", g_cpuNow);
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)t);
    }
}

void GuiSetGuardState(bool on)
{
    if (g_btnGuard) SetWindowTextW(g_btnGuard, on ? L"■ 停止守护" : L"▶ 启动守护");
    GuiRefreshCpuGrid();
}

void GuiNotifySession(bool active, DWORD pid)
{
    (void)active; (void)pid;
    GuiRefreshCpuGrid();
    GuiRefreshJournal();
}

// ---------------------------------------------------------------- 导出日志
static void ExportLog(int q, HWND parent)
{
    int n = LogLineCount(q);
    if (n == 0) { LogAction(L"[*] 日志为空，无需导出。"); return; }

    wchar_t file[MAX_PATH];
    wcscpy(file, q == 0 ? L"cs2opt-actions.txt" : L"cs2opt-diag.txt");
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = L"文本文件\0*.txt\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = L"txt";
    if (!GetSaveFileNameW(&ofn)) return;

    size_t cap = (size_t)n * 320 + 8;
    wchar_t* buf = (wchar_t*)calloc(cap, sizeof(wchar_t));
    if (!buf) return;
    wchar_t line[320];
    for (int i = 0; i < n; i++) {
        if (!LogGetLine(q, i, line, 320)) break;
        wcscat(buf, line);
        wcscat(buf, L"\r\n");
    }
    WriteFileAtomic(file, buf);
    free(buf);
    LogAction(L"[+] 日志已导出 (%d 行): %s", n, file);
}

// ---------------------------------------------------------------- 主窗口
static HWND g_hMain;

HWND GuiCreate(HINSTANCE hi, int nCmdShow)
{
    WNDCLASSEXW wcPage = { sizeof(wcPage) };
    wcPage.lpfnWndProc = PageProc; wcPage.hInstance = hi;
    wcPage.lpszClassName = L"CS2Page";
    wcPage.hbrBackground = g_brushDark;
    RegisterClassExW(&wcPage);
    WNDCLASSEXW wcG = { sizeof(wcG) };
    wcG.lpfnWndProc = GraphProc; wcG.hInstance = hi;
    wcG.lpszClassName = L"CS2Graph"; wcG.hbrBackground = g_brushDark;
    RegisterClassExW(&wcG);
    wcG.lpfnWndProc = CardsProc; wcG.lpszClassName = L"CS2Cards";
    RegisterClassExW(&wcG);
    wcG.lpfnWndProc = CoresProc; wcG.lpszClassName = L"CS2Cores";
    RegisterClassExW(&wcG);
    wcG.lpfnWndProc = LogProc; wcG.lpszClassName = L"CS2Log";
    RegisterClassExW(&wcG);
    WNDCLASSEXW wcM = { sizeof(wcM) };
    wcM.lpfnWndProc = MainProc; wcM.hInstance = hi;
    wcM.lpszClassName = L"CS2MainWnd";
    wcM.hbrBackground = g_brushDark;
    wcM.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcM.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wcM);

    int W = Sc(720), H = Sc(560);
    g_hMain = CreateWindowExW(0, L"CS2MainWnd", L"CS2 帧率与延迟优化面板 // 电竞版",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, W, H, NULL, NULL, hi, NULL);
    if (!g_hMain) return NULL;

    RECT rcInit; GetClientRect(g_hMain, &rcInit);
    g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER,
        0, 0, rcInit.right, rcInit.bottom, g_hMain, (HMENU)(INT_PTR)ID_TAB, hi, NULL);
    SendMessageW(g_tab, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    for (int i = 0; i < 7; i++) {
        TCITEMW ti; memset(&ti, 0, sizeof(ti));
        ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)kPages[i];
        TabCtrl_InsertItem(g_tab, i, &ti);
    }

    for (int i = 0; i < 7; i++) g_page[i] = MakePage(g_hMain, i);
    BuildPageOverview(g_page[0]);
    BuildPageTweaks(g_page[1]);
    BuildPageCpu(g_page[2]);
    BuildPageSuppress(g_page[3]);
    BuildPageCheckup(g_page[4]);
    BuildPageRestore(g_page[5]);
    BuildPageLog(g_page[6]);

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL,
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_hMain,
        (HMENU)(INT_PTR)ID_STATUS, hi, NULL);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_fUi, TRUE);

    for (int i = 1; i < 7; i++) ShowWindow(g_page[i], SW_HIDE);
    ShowWindow(g_page[0], SW_SHOW);

    GuiLogSetWnd(g_hMain);
    LogAction(L"=== CS2 优化器 " APP_VERSION L" 面板就绪 (纯 C / Win32 / 不触碰游戏内存) ===");
    LogAction(IsRunAsAdmin() ? L"[*] 已具备管理员权限。"
                             : L"[!] 普通权限：需管理员的优化项会标注，请以管理员重启工具。");
    GuiRefreshTweaks();
    GuiRefreshWhitelist();
    GuiRefreshJournal();
    GuiSetGuardState(InterlockedCompareExchange(&g_guardActive, 0, 0) != 0);

    ShowWindow(g_hMain, nCmdShow);
    UpdateWindow(g_hMain);
    return g_hMain;
}

// ---------------------------------------------------------------- 页面切换
static void ShowPage(int idx)
{
    for (int i = 0; i < 7; i++)
        ShowWindow(g_page[i], i == idx ? SW_SHOW : SW_HIDE);
    g_curPage = idx;
    if (idx == 1) GuiRefreshTweaks();
    if (idx == 2) GuiRefreshCpuGrid();
    if (idx == 3) GuiRefreshSuppressList();
    if (idx == 4) { if (g_findN == 0) g_findN = CheckupRun(g_findings, 64);
                    GuiRefreshCheckup(); }
    if (idx == 5) GuiRefreshJournal();
    if (idx == 6) {
        GuiLogDrain(NULL, NULL);
        if (g_edAction) InvalidateRect(g_edAction, NULL, FALSE);
        if (g_edDiag) InvalidateRect(g_edDiag, NULL, FALSE);
    }
}

void GuiShowPage(int idx)
{
    if (idx < 0 || idx > 6) return;
    if (g_tab) SendMessageW(g_tab, 0x130C, (WPARAM)idx, 0);  // TCM_SETCURSEL
    ShowPage(idx);
}

static void OnTweaksApplyChecked(void)
{
    int n; const RegTweak* t = TweakTable(&n);
    int on = 0, off = 0, skip = 0;
    for (int i = 0; i < n; i++) {
        bool want = ListView_GetCheckState(g_lvTweaks, i) != 0;
        bool have = TweakIsApplied(&t[i]);
        if (want == have) continue;                       // 已是目标状态
        if (want) {
            if (t[i].needsAdmin && !IsRunAsAdmin()) {
                LogAction(L"[-] %s 需要管理员权限，跳过。", t[i].label);
                skip++;
                continue;
            }
            if (TweakApply(&t[i])) on++;
        } else {
            if (TweakRevert(&t[i])) off++;
        }
    }
    LogAction(L"勾选项同步完成：启用 %d 项，还原 %d 项，跳过 %d 项", on, off, skip);
    GuiRefreshTweaks();
    GuiRefreshJournal();
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        int tabH = Sc(30);
        if (g_tab) MoveWindow(g_tab, 0, 0, rc.right, rc.bottom, TRUE);
        for (int i = 0; i < 7; i++)
            if (g_page[i]) MoveWindow(g_page[i], 0, tabH, rc.right, rc.bottom - tabH - Sc(24), TRUE);
        MoveWindow(g_status, 0, rc.bottom - Sc(24), rc.right, Sc(24), TRUE);
        return 0;
    }
    case WM_DPICHANGED: {
        LPRECT r = (LPRECT)lParam;
        SetWindowPos(hwnd, NULL, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        g_dpi = HIWORD(wParam);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        LPMINMAXINFO m = (LPMINMAXINFO)lParam;
        m->ptMinTrackSize.x = Sc(700);
        m->ptMinTrackSize.y = Sc(520);
        return 0;
    }
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lParam;
        if (nm->hwndFrom == g_tab && nm->code == TCN_SELCHANGE)
            ShowPage(TabCtrl_GetCurSel(g_tab));
        if (nm->hwndFrom == g_lvCheck && nm->code == NM_CUSTOMDRAW) {
            LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lParam;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int lvl = (int)cd->nmcd.lItemlParam;
                cd->clrText = lvl == 0 ? RGB(0,120,60) :
                              lvl == 2 ? RGB(200,60,20) :
                              lvl == 1 ? RGB(30,90,180) : RGB(120,120,120);
                return CDRF_NEWFONT;
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case ID_BTN_GUARD:
            if (InterlockedCompareExchange(&g_guardActive, 0, 0) == 0) GuardStart();
            else GuardStop();
            break;
        case ID_RB_SMART: g_set.mode = 0; SettingsSave(); break;
        case ID_RB_COMP:  g_set.mode = 1; SettingsSave(); break;
        case ID_CB_INTENSITY:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                g_set.intensity = (int)SendMessageW(g_cbIntensity, CB_GETCURSEL, 0, 0);
                SettingsSave();
            }
            break;
        case ID_CB_TIMERS:
            g_set.timers = (SendMessageW(g_cbTimers, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
            GuardSetTimers(g_set.timers != 0);
            SettingsSave();
            break;
        case ID_CB_POLICY:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                g_set.policy = (int)SendMessageW(g_cbPolicy, CB_GETCURSEL, 0, 0);
                SettingsSave();
                LONG pid = InterlockedCompareExchange(&g_gamePid, 0, 0);
                if (pid) ApplyAffinityPolicy((DWORD)pid, g_set.policy);
                GuiRefreshCpuGrid();
            }
            break;
        case ID_BTN_APPLY_SEL:  OnTweaksApplyChecked(); break;
        case ID_BTN_APPLY_ALL:  TweaksApplyAll(); GuiRefreshTweaks(); break;
        case ID_BTN_REVERT_PERSIST: TweaksRevertPersistent(); GuiRefreshTweaks(); GuiRefreshJournal(); break;
        case ID_BTN_POWER:      PowerApplyHighPerformance(); GuiRefreshJournal(); break;
        case ID_BTN_FSO:        FsoDisableForCs2(); GuiRefreshJournal(); break;
        case ID_BTN_HAGS:       HagsToggle(); GuiRefreshJournal(); break;
        case ID_BTN_CFG:        CfgGenerateAutoexec(); break;
        case ID_BTN_TUNE_NOW:   TuneCs2Now(false); GuiRefreshCpuGrid(); GuiRefreshJournal(); break;
        case ID_BTN_WSLOCK:     PrewarmCs2WorkingSet(); break;
        case ID_BTN_WHITE_ADD: {
            HWND ed = GetDlgItem(g_page[3], ID_ED_WHITE);
            wchar_t txt[80] = L"";
            GetWindowTextW(ed, txt, 79);
            if (txt[0]) {
                WhitelistAdd(txt);
                GuiRefreshWhitelist();
                LogAction(L"[+] 白名单添加: %s", txt);
                SetWindowTextW(ed, L"");
            }
            break;
        }
        case ID_BTN_WHITE_DEL: {
            int sel = (int)SendMessageW(g_lbWhite, LB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                if (WhitelistRemoveAt(sel)) { GuiRefreshWhitelist(); LogAction(L"[+] 白名单已移除一项"); }
                else LogAction(L"[-] 该项为核心保护，不可移除。");
            }
            break;
        }
        case ID_BTN_RUN_CHECK:
            g_findN = CheckupRun(g_findings, 64);
            GuiRefreshCheckup();
            break;
        case ID_BTN_FIX_ALL:
            if (g_findN == 0) { LogAction(L"[*] 请先运行体检。"); break; }
            CheckupFixAll(g_findings, g_findN);
            g_findN = CheckupRun(g_findings, 64);
            GuiRefreshCheckup();
            GuiRefreshTweaks();
            GuiRefreshJournal();
            break;
        case ID_BTN_RS_SESSION: JournalRestore(false, true, L"手动还原会话"); GuiRefreshJournal(); break;
        case ID_BTN_RS_PERSIST: JournalRestore(true, false, L"手动还原持久"); GuiRefreshJournal(); GuiRefreshTweaks(); break;
        case ID_BTN_RS_ALL:     JournalRestore(false, false, L"手动全部还原"); GuiRefreshJournal(); GuiRefreshTweaks(); break;
        case ID_BTN_J_CLEAR:
            if (MessageBoxW(hwnd, L"清空 journal 将放弃所有可还原记录（已应用的系统改动不会被恢复）。\n确定？",
                            L"危险操作", MB_YESNO | MB_ICONWARNING) == IDYES) {
                JournalClearDisk();
                GuiRefreshJournal();
                LogAction(L"[!] journal 已清空");
            }
            break;
        case ID_BTN_EXPORT_A: ExportLog(0, hwnd); break;
        case ID_BTN_EXPORT_D: ExportLog(1, hwnd); break;
        case ID_BTN_LOG_CLEAR:
            LogClearMemory(0);
            LogClearMemory(1);
            if (g_edAction) InvalidateRect(g_edAction, NULL, FALSE);
            if (g_edDiag) InvalidateRect(g_edDiag, NULL, FALSE);
            LogAction(L"[*] 显示缓冲已清空（磁盘日志仍保留）");
            break;
        }
        GuiLogDrain(NULL, NULL);
        return 0;
    }
    case WM_APP_LOGQ:
        GuiLogDrain(NULL, NULL);          // 只清队，绘制由自绘控件负责
        if (g_curPage == 6) {
            if (g_edAction) InvalidateRect(g_edAction, NULL, FALSE);
            if (g_edDiag) InvalidateRect(g_edDiag, NULL, FALSE);
        }
        return 0;
    case WM_APP + 9:
        GuiShowPage((int)wParam);
        return 0;
    case WM_APP_SESS:
        GuiRefreshCpuGrid();
        GuiRefreshJournal();
        return 0;
    case WM_TIMER:
        GuiOnTick();
        LogFlushToFiles();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
