// main.c — 入口 / 守护状态机 / 会话生命周期 / 崩溃恢复 / 提权续跑
#include "app.h"

volatile LONG g_guardActive = 0;
volatile LONG g_gameActive  = 0;
volatile LONG g_gamePid     = 0;

static HANDLE g_evStop = NULL, g_hGuardThread = NULL;
static volatile LONG g_timerOn = 0;

// ---------------------------------------------------------------- 守护线程
extern HWND g_hMainWndPublic;

static DWORD WINAPI GuardThread(LPVOID param)
{
    (void)param;
    DWORD lastPid = 0;
    bool sessionOpen = false;

    for (;;) {
        DWORD w = WaitForSingleObject(g_evStop, 2000);
        if (w == WAIT_OBJECT_0) break;

        DWORD pid = FindCs2Process(NULL, 0);

        if (pid != 0 && pid != lastPid) {
            lastPid = pid;
            InterlockedExchange(&g_gamePid, (LONG)pid);
            InterlockedExchange(&g_gameActive, 1);
            LogAction(L"[!] 检测到 cs2.exe (PID: %lu)，开启会话…", pid);
            if (!sessionOpen) {
                sessionOpen = true;
                JournalEntry* ex = NULL;
                if (!JournalFindByKey(JK_POWERPLAN, L"power", NULL, &ex))
                    PowerApplyHighPerformance();      // 会话电源
                TuneCs2Now(true);                     // 优先级/亲和性/EcoQoS
            }
            if (g_hMainWndPublic)
                PostMessageW(g_hMainWndPublic, WM_APP_SESS, 1, (LPARAM)pid);
        } else if (pid == 0 && lastPid != 0) {
            lastPid = 0;
            InterlockedExchange(&g_gamePid, 0);
            InterlockedExchange(&g_gameActive, 0);
            LogAction(L"[*] cs2.exe 已退出，还原会话…");
            SuppressRestoreSession();
            JournalRestore(false, true, L"游戏退出");  // 会话项全部还原
            sessionOpen = false;
            if (g_hMainWndPublic)
                PostMessageW(g_hMainWndPublic, WM_APP_SESS, 0, 0);
        }

        SuppressTick(lastPid != 0);
    }
    return 0;
}

// ---------------------------------------------------------------- 守护控制
void GuardSetTimers(bool on)
{
    bool active = InterlockedCompareExchange(&g_guardActive, 0, 0) != 0;
    if (on && active && InterlockedCompareExchange(&g_timerOn, 0, 0) == 0) {
        timeBeginPeriod(1);
        InterlockedExchange(&g_timerOn, 1);
        LogDiag(L"timeBeginPeriod(1)");
    } else if ((!on || !active) && InterlockedCompareExchange(&g_timerOn, 0, 0) == 1) {
        timeEndPeriod(1);
        InterlockedExchange(&g_timerOn, 0);
        LogDiag(L"timeEndPeriod(1)");
    }
}

void GuardStart(void)
{
    if (g_hGuardThread) return;
    if (!g_evStop) g_evStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    ResetEvent(g_evStop);
    InterlockedExchange(&g_guardActive, 1);
    PowerCaptureBaseline();          // 抢占前先记住基线, 防止外部电源工具污染还原值
    g_hGuardThread = CreateThread(NULL, 0, GuardThread, NULL, 0, NULL);
    if (!g_hGuardThread) {
        InterlockedExchange(&g_guardActive, 0);
        LogAction(L"[-] 守护线程创建失败");
        return;
    }
    GuardSetTimers(g_set.timers != 0);
    LogAction(L"[守护] 启动 (2s 轮询 cs2.exe + 后台治理)");
    GuiSetGuardState(true);
}

void GuardStop(void)
{
    if (g_hGuardThread) {
        SetEvent(g_evStop);
        WaitForSingleObject(g_hGuardThread, 4000);
        CloseHandle(g_hGuardThread);
        g_hGuardThread = NULL;
    }
    InterlockedExchange(&g_guardActive, 0);
    GuardSetTimers(false);
    SuppressRestoreSession();
    JournalRestore(false, true, L"守护停止");
    InterlockedExchange(&g_gamePid, 0);
    InterlockedExchange(&g_gameActive, 0);
    LogAction(L"[守护] 已停止，会话项已还原");
    GuiSetGuardState(false);
}

// ---------------------------------------------------------------- 入口
HWND g_hMainWndPublic = NULL;

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR pCmdLine, int nCmdShow)
{
    (void)hPrev; (void)pCmdLine;

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"CS2Optimizer_SingleInstance_Mutex");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"CS2 优化器已在运行 (可能位于托盘/其他桌面)。",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    {   // PMv2 尽力而为 (主靠嵌入清单)
        HMODULE u = GetModuleHandleW(L"user32");
        FARPROC f = u ? GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
        if (f) ((BOOL(WINAPI*)(HANDLE))f)((HANDLE)(INT_PTR)-4);
    }
    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    AppDirsInit();
    LogInit();
    TopoQuery();
    SettingsLoad();
    WhitelistInit();
    GuiInitGfx();                 // 字体先于 journal 日志 (LogDiag 需要)
    JournalLoadAtBoot();          // 崩溃/断电恢复：先于 GUI 落盘

    HWND hwnd = GuiCreate(hInstance, nCmdShow);
    if (!hwnd) return 1;
    g_hMainWndPublic = hwnd;

    SetTimer(hwnd, 77, 1000, NULL);

    // --autoclick 提权续跑 / --page 直达页 (自动化测试用)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (_wcsnicmp(argv[i], L"--autoclick=", 12) == 0) {
                int id = _wtoi(argv[i] + 12);
                if (id > 0) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(id, 0), 0);
            } else if (_wcsnicmp(argv[i], L"--page=", 7) == 0) {
                int pg = _wtoi(argv[i] + 7);
                PostMessageW(hwnd, WM_APP + 9, (WPARAM)pg, 0);
            }
        }
        LocalFree(argv);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GuardStop();                  // 兜底：窗口异常路径退出也还原
    LogFlushToFiles();
    if (g_evStop) CloseHandle(g_evStop);
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
