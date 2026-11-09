// proc.c — CS2 进程发现 / 即时调优 / 后台智能抑制引擎 (Pavise 核心范式)
#include "app.h"

// ---------------------------------------------------------------- 进程发现
DWORD FindCs2Process(wchar_t* outPath, size_t cchPath)
{
    if (outPath && cchPath) outPath[0] = 0;
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"cs2.exe") == 0) {
                pid = pe.th32ProcessID;
                if (outPath && cchPath > 1) {
                    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                            FALSE, pid);
                    if (hp) {
                        DWORD n = (DWORD)cchPath;
                        if (!QueryFullProcessImageNameW(hp, 0, outPath, &n))
                            outPath[0] = 0;
                        CloseHandle(hp);
                    }
                }
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

HANDLE OpenCs2(DWORD pid)
{
    return OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION |
                       PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}

static uint64_t ProcCreateTime(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(h, &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER ul; ul.LowPart = c.dwLowDateTime; ul.HighPart = c.dwHighDateTime;
    return ul.QuadPart;
}

// ---------------------------------------------------------------- 亲和性
void ApplyAffinityPolicy(DWORD pid, int policy)
{
    HANDLE h = OpenCs2(pid);
    if (!h) return;
    DWORD_PTR procMask = 0, sysMask = 0;
    if (!GetProcessAffinityMask(h, &procMask, &sysMask)) {
        LogDiagWin(L"GetProcessAffinityMask");
        CloseHandle(h); return;
    }
    uint64_t ct = ProcCreateTime(h);

    DWORD_PTR target = 0;
    const wchar_t* label = L"";
    switch (policy) {
    case AFF_KEEP_ALL:
        CloseHandle(h); return;
    case AFF_PHYSICAL_ONLY:
        if (g_topo.ok) { target = g_topo.physOnePerCoreMask; label = L"剔除超线程"; }
        break;
    case AFF_DROP_ECORES:
        if (g_topo.ok && g_topo.hybrid) { target = g_topo.pCoreMask; label = L"剔除 E-core"; }
        else LogAction(L"[=] 非大小核架构，跳过 E-core 剔除。");
        break;
    case AFF_ALL_CORES:
        target = g_topo.ok ? g_topo.allMask : sysMask;
        label = L"全部核心";
        break;
    case AFF_CORE0:
        target = sysMask & ~(DWORD_PTR)3; label = L"剔除逻辑核 0/1";
        break;
    }
    target &= procMask;
    if (target == 0 || target == procMask) { CloseHandle(h); return; }

    if (SetProcessAffinityMask(h, target)) {
        LogAction(L"[+] CS2 亲和性 (%s): 0x%llX", label, (unsigned long long)target);
        if (!JournalFindByKey(JK_AFFINITY, L"pid", NULL, NULL)) {
            JournalEntry je; memset(&je, 0, sizeof(je));
            je.kind = JK_AFFINITY; je.persistent = false; je.applied = true;
            wcscpy(je.key, L"pid"); wcscpy(je.name, L"CS2 亲和性");
            je.oldDword = procMask; je.newDword = target;
            je.pid = pid; je.createTime = ct;
            JournalAdd(&je);
        }
    } else {
        LogDiagWin(L"SetProcessAffinityMask");
    }
    CloseHandle(h);
}

void Cs2AffinityRestore(DWORD pid)
{
    JournalEntry* j = NULL;
    if (JournalFindByKey(JK_AFFINITY, L"pid", NULL, &j) && j && j->pid == pid) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
        if (h) { SetProcessAffinityMask(h, (DWORD_PTR)j->oldDword); CloseHandle(h); }
    }
}

// ---------------------------------------------------------------- 即时调优
void TuneCs2Now(bool fromAuto)
{
    DWORD pid = FindCs2Process(NULL, 0);
    if (!pid) { if (!fromAuto) LogAction(L"[-] CS2 未运行。"); return; }

    HANDLE h = OpenCs2(pid);
    if (!h) {
        if (!fromAuto) LogAction(L"[-] 打开 cs2.exe 失败 (权限不足时请以管理员运行)。");
        LogDiagWin(L"OpenCs2");
        return;
    }
    uint64_t ct = ProcCreateTime(h);

    // 优先级 (journal 记录原始值)
    DWORD oldPri = GetPriorityClass(h);
    if (SetPriorityClass(h, HIGH_PRIORITY_CLASS) && oldPri != HIGH_PRIORITY_CLASS) {
        if (!JournalFindByKey(JK_PRIORITY, L"pid", NULL, NULL)) {
            JournalEntry je; memset(&je, 0, sizeof(je));
            je.kind = JK_PRIORITY; je.persistent = false; je.applied = true;
            wcscpy(je.key, L"pid"); wcscpy(je.name, L"CS2 优先级");
            je.oldDword = oldPri ? oldPri : NORMAL_PRIORITY_CLASS;
            je.newDword = HIGH_PRIORITY_CLASS;
            je.pid = pid; je.createTime = ct;
            JournalAdd(&je);
        }
        if (!fromAuto) LogAction(L"[+] CS2 (PID: %lu) 优先级 -> 高", pid);
    }

    // EcoQoS: 解除节流 + 定时器精度
    PROCESS_POWER_THROTTLING_STATE t;
    memset(&t, 0, sizeof(t));
    t.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    t.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED
                  | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    t.StateMask = 0;
    if (SetProcessInformation(h, ProcessPowerThrottling, &t, sizeof(t))) {
        if (!JournalFindByKey(JK_ECOQOS, L"pid", NULL, NULL)) {
            JournalEntry je; memset(&je, 0, sizeof(je));
            je.kind = JK_ECOQOS; je.persistent = false; je.applied = true;
            wcscpy(je.key, L"pid"); wcscpy(je.name, L"CS2 电源节流");
            je.pid = pid; je.createTime = ct;
            JournalAdd(&je);
        }
    } else LogDiagWin(L"CS2 PowerThrottling");

    CloseHandle(h);
    ApplyAffinityPolicy(pid, g_set.policy);
}

// ---------------------------------------------------------------- 后台抑制
// 借鉴 Pavise：对"可管理后台"降低 CPU/IO/内存调度优先级 + EcoQoS 效率模式；
// 游戏链/系统关键/白名单进程永不触碰；游戏退出后逐项还原 (journal)。
// 竞技模式 (mode=1) 持续治理；Smart 模式 (mode=0) 仅游戏前台时治理。

typedef struct {
    DWORD    pid;
    uint64_t createTime;
    DWORD    oldPri;
    DWORD    oldMemPri;
    bool     hadEco;
    bool     valid;
    wchar_t  name[32];
} SuppressedProc;

// EnumWindows 判定：该 pid 是否拥有可见顶层窗口 (浏览器/启动器/更新器典型形态)。
// 服务、无窗口后台一律不治理 —— 比 Pavise 更保守的安全边界。
typedef struct { DWORD pid; bool found; } WinEnumCtx;
static BOOL CALLBACK HasWindowProc(HWND h, LPARAM l)
{
    WinEnumCtx* ctx = (WinEnumCtx*)l;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == ctx->pid && IsWindowVisible(h) &&
        GetWindow(h, GW_OWNER) == NULL && GetWindowTextLengthW(h) >= 0) {
        LONG style = (LONG)GetWindowLongPtrW(h, GWL_STYLE);
        if (style & WS_VISIBLE) { ctx->found = true; return FALSE; }
    }
    return TRUE;
}
static bool PidHasVisibleWindow(DWORD pid)
{
    WinEnumCtx ctx = { pid, false };
    EnumWindows(HasWindowProc, (LPARAM)&ctx);
    return ctx.found;
}

#define SUP_MAX 64
static SuppressedProc g_sup[SUP_MAX];
static CRITICAL_SECTION g_supCs;
static bool g_supCsInit;

static void SupCs(void)
{
    if (!g_supCsInit) { InitializeCriticalSection(&g_supCs); g_supCsInit = true; }
}

int SuppressActiveCount(void)
{
    int n = 0;
    SupCs();
    EnterCriticalSection(&g_supCs);
    for (int i = 0; i < SUP_MAX; i++) if (g_sup[i].valid) n++;
    LeaveCriticalSection(&g_supCs);
    return n;
}

// 供 GUI 展示当前被抑制进程：pid + exe 名
int SuppressSnapshot(DWORD* pids, wchar_t names[][32], int max)
{
    int n = 0;
    SupCs();
    EnterCriticalSection(&g_supCs);
    for (int i = 0; i < SUP_MAX && n < max; i++) {
        if (!g_sup[i].valid) continue;
        pids[n] = g_sup[i].pid;
        wcsncpy(names[n], g_sup[i].name[0] ? g_sup[i].name : L"(后台)", 31);
        names[n][31] = 0;
        n++;
    }
    LeaveCriticalSection(&g_supCs);
    return n;
}

static bool GameForeground(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgp = 0;
    GetWindowThreadProcessId(fg, &fgp);
    LONG gp = InterlockedCompareExchange(&g_gamePid, 0, 0);
    if (fgp == (DWORD)gp) return true;
    // Steam 大窗口模式：前台是 steam.exe 且 CS2 在运行
    wchar_t title[128];
    if (GetWindowTextW(fg, title, 128) > 0) {
        if (wcsstr(title, L"Counter-Strike") || wcsstr(title, L"CS2") ||
            wcsstr(title, L"反恐精英")) return true;
    }
    return false;
}

static void SuppressOne(DWORD pid, const wchar_t* exeName, const wchar_t* fullExe,
                        int intensity)
{
    SupCs();
    EnterCriticalSection(&g_supCs);
    int slot = -1;
    for (int i = 0; i < SUP_MAX; i++) {
        if (g_sup[i].valid && g_sup[i].pid == pid) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < SUP_MAX; i++) if (!g_sup[i].valid) { slot = i; break; }
    }
    if (slot < 0) { LeaveCriticalSection(&g_supCs); return; }

    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION |
                           PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_LIMITED_INFORMATION,
                           FALSE, pid);
    if (!h) { LeaveCriticalSection(&g_supCs); return; }

    SuppressedProc* s = &g_sup[slot];
    s->pid = pid;
    s->createTime = ProcCreateTime(h);
    s->oldPri = GetPriorityClass(h);
    if (!s->oldPri) s->oldPri = NORMAL_PRIORITY_CLASS;
    wcsncpy(s->name, exeName, 31); s->name[31] = 0;

    // 内存优先级
    MEMORY_PRIORITY_INFORMATION mpi;
    s->oldMemPri = 5;   // MEMORY_PRIORITY_NORMAL
    if (GetProcessInformation(h, ProcessMemoryPriority, &mpi, sizeof(mpi)))
        s->oldMemPri = mpi.MemoryPriority;

    // CPU 优先级: 普通->低于正常; 激进->空闲
    DWORD newPri = (intensity >= 2) ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS;
    if (s->oldPri != newPri)
        SetPriorityClass(h, newPri);

    // 内存调度降级
    if (s->oldMemPri > 4) {
        MEMORY_PRIORITY_INFORMATION m; m.MemoryPriority = 4; // LOW
        SetProcessInformation(h, ProcessMemoryPriority, &m, sizeof(m));
    }

    // EcoQoS 效率模式 (激进档才开，避免误伤后台工具)
    s->hadEco = false;
    if (intensity >= 2) {
        PROCESS_POWER_THROTTLING_STATE t;
        memset(&t, 0, sizeof(t));
        t.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        t.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        t.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        if (SetProcessInformation(h, ProcessPowerThrottling, &t, sizeof(t)))
            s->hadEco = true;
    }
    s->valid = true;
    LeaveCriticalSection(&g_supCs);
    CloseHandle(h);

    InterlockedIncrement(&g_suppressedCount);
    LogDiag(L"抑制后台: %s (PID %lu) pri %lu->%lu", exeName, pid,
            s->oldPri, newPri);
    (void)fullExe;
}

static void RestoreSuppressed(const SuppressedProc* s)
{
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION |
                           PROCESS_SET_LIMITED_INFORMATION, FALSE, s->pid);
    if (!h) return;
    FILETIME c, e, k, u;
    if (GetProcessTimes(h, &c, &e, &k, &u)) {
        ULARGE_INTEGER ul; ul.LowPart = c.dwLowDateTime; ul.HighPart = c.dwHighDateTime;
        if (s->createTime == 0 || ul.QuadPart == s->createTime) {
            // 精确还原原值 (Pavise 式 restoration ownership)
            if (s->oldPri && s->oldPri != GetPriorityClass(h))
                SetPriorityClass(h, s->oldPri);
            MEMORY_PRIORITY_INFORMATION m; m.MemoryPriority = s->oldMemPri;
            SetProcessInformation(h, ProcessMemoryPriority, &m, sizeof(m));
            if (s->hadEco) {
                PROCESS_POWER_THROTTLING_STATE t;
                memset(&t, 0, sizeof(t));
                t.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                t.ControlMask = 0; t.StateMask = 0;
                SetProcessInformation(h, ProcessPowerThrottling, &t, sizeof(t));
            }
        }
    }
    CloseHandle(h);
}

void SuppressRestoreSession(void)
{
    SupCs();
    EnterCriticalSection(&g_supCs);
    int n = 0;
    for (int i = 0; i < SUP_MAX; i++) {
        if (g_sup[i].valid) {
            RestoreSuppressed(&g_sup[i]);
            g_sup[i].valid = false;
            n++;
        }
    }
    LeaveCriticalSection(&g_supCs);
    if (n > 0) {
        InterlockedExchangeAdd(&g_restoredCount, n);
        LogAction(L"[还原] %d 个后台进程调度已恢复", n);
    }
}

// 判断"可管理后台"：必须有可见顶层窗口 (桌面应用形态)，且非系统关键、
// 非白名单、非游戏链、非本工具。无窗口服务进程永不触碰。
static bool EligibleBackground(const wchar_t* exeName, const wchar_t* fullPath,
                               DWORD pid)
{
    if (IsSystemCritical(exeName)) return false;
    if (WhitelistContains(exeName, fullPath)) return false;
    if (IsGameProcess(pid, exeName)) return false;
    DWORD myPid = GetCurrentProcessId();
    if (pid == myPid) return false;
    if (!PidHasVisibleWindow(pid)) return false;
    return true;
}

void SuppressTick(bool gameActive)
{
    int intensity = g_set.intensity;
    if (!gameActive || intensity == 0) {
        // Smart: 游戏不在前台/退出 -> 释放治理
        SuppressRestoreSession();
        return;
    }
    if (g_set.mode == 0 && !GameForeground()) {
        SuppressRestoreSession();   // Smart 模式切走释放；竞技模式持续
        return;
    }

    // 清理已消失的条目
    SupCs();
    EnterCriticalSection(&g_supCs);
    for (int i = 0; i < SUP_MAX; i++) {
        if (!g_sup[i].valid) continue;
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, g_sup[i].pid);
        if (!h) g_sup[i].valid = false;
        else CloseHandle(h);
    }
    LeaveCriticalSection(&g_supCs);

    // 枚举并抑制
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            DWORD pid = pe.th32ProcessID;
            if (pid == 0 || pid == 4) continue;
            if (_wcsicmp(pe.szExeFile, L"cs2.exe") == 0) continue;
            // 已在抑制表则跳过
            bool already = false;
            EnterCriticalSection(&g_supCs);
            for (int i = 0; i < SUP_MAX; i++)
                if (g_sup[i].valid && g_sup[i].pid == pid) { already = true; break; }
            LeaveCriticalSection(&g_supCs);
            if (already) continue;

            wchar_t full[MAX_PATH * 2] = L"";
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hp) {
                DWORD n = (DWORD)(sizeof(full)/sizeof(wchar_t));
                QueryFullProcessImageNameW(hp, 0, full, &n);
                CloseHandle(hp);
            }
            if (!EligibleBackground(pe.szExeFile, full, pid)) continue;

            // 负载门槛 (Smart 模式只在高负载扩展治理; 竞技模式全治理)
            if (g_set.mode == 0) {
                // 粗略: 前台游戏活跃即视为高负载场景，抑制窗口类后台
            }
            SuppressOne(pid, pe.szExeFile, full, intensity);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}
