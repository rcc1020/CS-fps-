// tweaks.c — 表驱动系统优化项 (查询/应用/还原走 journal 事务) + 电源/FSO/HAGS/cfg
#include "app.h"

// ---------------------------------------------------------------- 优化表
// 每项 = 一个注册表 DWORD。应用前把旧值记入 journal (磁盘持久化)，
// 崩溃/重启后 JournalLoadAtBoot 自动还原；面板"还原中心"可手动还原。
static const RegTweak kTable[] = {
    { L"gamedvr_enabled", L"禁用 Xbox GameDVR 后台录制",
      L"GameConfigStore\\GameDVR_Enabled = 0",
      HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_Enabled", 0,
      FALSE, FALSE },
    { L"gamedvr_capture", L"禁用 GameDVR 应用捕获",
      L"GameDVR\\AppCaptureEnabled = 0",
      HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
      L"AppCaptureEnabled", 0, FALSE, FALSE },
    { L"gamebar", L"禁用 Xbox Game Bar",
      L"GameDVR\\GameBarEnabled = 0",
      HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
      L"GameBarEnabled", 0, FALSE, FALSE },
    { L"fse_mode", L"GameDVR 全屏行为模式=2",
      L"GameConfigStore\\GameDVR_FSEBehaviorMode = 2 (防全屏被拉回)",
      HKEY_CURRENT_USER, L"System\\GameConfigStore",
      L"GameDVR_FSEBehaviorMode", 2, FALSE, FALSE },
    { L"net_throttle", L"解除多媒体网络节流",
      L"NetworkThrottlingIndex = 0xFFFFFFFF",
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
      L"NetworkThrottlingIndex", 0xFFFFFFFF, TRUE, FALSE },
    { L"sys_resp", L"清零系统 CPU 预留",
      L"SystemResponsiveness = 0",
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
      L"SystemResponsiveness", 0, TRUE, FALSE },
    { L"prio_sep", L"前台时间片: 短固定 6:1",
      L"Win32PrioritySeparation = 0x26 (游戏前台更稳)",
      HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl",
      L"Win32PrioritySeparation", 0x26, TRUE, FALSE },
    { L"ntfs_83", L"关闭 NTFS 8.3 短名生成",
      L"NtfsDisable8dot3NameCreation = 1",
      HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
      L"NtfsDisable8dot3NameCreation", 1, TRUE, FALSE },
    { L"ntfs_last", L"关闭 NTFS 最后访问时间更新",
      L"NtfsDisableLastAccessUpdate = 1",
      HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
      L"NtfsDisableLastAccessUpdate", 1, TRUE, FALSE },
    { L"memcomp", L"关闭内存压缩 (降 DPC 抖动)",
      L"DisableMemoryCompression = 1 (需重启)",
      HKEY_LOCAL_MACHINE,
      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
      L"DisableMemoryCompression", 1, TRUE, TRUE },
};

const RegTweak* TweakTable(int* count)
{
    if (count) *count = (int)(sizeof(kTable) / sizeof(kTable[0]));
    return kTable;
}

static void MakeRegKey(wchar_t* out, size_t cch, const RegTweak* t)
{
    swprintf(out, cch, L"%s|%s|%s",
             t->hive == HKEY_LOCAL_MACHINE ? L"HKLM" : L"HKCU", t->sub, t->val);
}

bool TweakIsApplied(const RegTweak* t)
{
    HKEY hk;
    if (RegOpenKeyExW(t->hive, t->sub, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    DWORD v = 0, type = 0, cb = sizeof(v);
    LONG r = RegQueryValueExW(hk, t->val, NULL, &type, (BYTE*)&v, &cb);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS && type == REG_DWORD && v == t->value;
}

bool TweakApply(const RegTweak* t)
{
    HKEY hk;
    LONG r = RegOpenKeyExW(t->hive, t->sub, 0, KEY_READ | KEY_WRITE, &hk);
    if (r != ERROR_SUCCESS) {
        r = RegCreateKeyExW(t->hive, t->sub, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_READ | KEY_WRITE, NULL, &hk, NULL);
        if (r != ERROR_SUCCESS) {
            LogDiagWin(t->label);
            return false;
        }
    }
    DWORD old = 0, type = 0, cb = sizeof(old);
    LONG q = RegQueryValueExW(hk, t->val, NULL, &type, (BYTE*)&old, &cb);
    bool missing = (q != ERROR_SUCCESS || type != REG_DWORD);

    r = RegSetValueExW(hk, t->val, 0, REG_DWORD,
                       (const BYTE*)&t->value, sizeof(t->value));
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS) { LogDiagWin(t->label); return false; }

    // 记入 journal (持久项)
    JournalEntry je; memset(&je, 0, sizeof(je));
    je.kind = JK_REG_DWORD;
    je.persistent = true;
    je.applied = true;
    MakeRegKey(je.key, 260, t);
    wcsncpy(je.name, t->label, 79);
    je.oldDword = missing ? 0xDEAD0001ull : old;
    je.newDword = t->value;
    JournalAdd(&je);

    LogAction(L"[+] %s%s", t->label, t->needsReboot ? L" (需重启生效)" : L"");
    return true;
}

bool TweakRevert(const RegTweak* t)
{
    wchar_t key[260];
    MakeRegKey(key, 260, t);
    if (JournalRevertRegKey(key)) return true;
    LogDiag(L"%s 无 journal 旧值，跳过还原", t->label);
    return false;
}

void TweaksApplyAll(void)
{
    int n; const RegTweak* t = TweakTable(&n);
    bool needAdmin = false;
    int done = 0, skipped = 0;
    for (int i = 0; i < n; i++) {
        if (TweakIsApplied(&t[i])) { skipped++; continue; }
        if (t[i].needsAdmin && !IsRunAsAdmin()) { needAdmin = true; continue; }
        if (TweakApply(&t[i])) done++;
    }
    LogAction(L"—— 系统优化: 新应用 %d 项, 已是目标值 %d 项 ——", done, skipped);
    if (needAdmin)
        LogAction(L"[!] 有需管理员的项目未应用：请以管理员权限重新打开工具。");
}

void TweaksRevertPersistent(void)
{
    JournalRestore(true, false, L"还原持久优化");
}

// ---------------------------------------------------------------- 电源方案
// 关键: 电源基线必须在"守护启动时"采集, 而不是等到检测到 cs2.exe 才读当前值。
// 原因: ProcessLasso / Bitsum / 主板厂商工具会在游戏进程启动瞬间抢先切方案,
//       若那时才读"旧值", 记录到的已是对手的方案, 退出后就会还原成对手的值。
static wchar_t g_powerBaseline[64];
static bool    g_powerBaselineValid = false;

void PowerCaptureBaseline(void)
{
    GUID* g = NULL;
    if (PowerGetActiveScheme(NULL, &g) == ERROR_SUCCESS && g) {
        FormatGuidText(g, g_powerBaseline, 64);
        g_powerBaselineValid = true;
        LocalFree(g);
        LogDiag(L"采集电源基线: %s", g_powerBaseline);
    } else {
        LogDiagWin(L"PowerGetActiveScheme (基线)");
    }
}

void PowerForgetBaseline(void)
{
    g_powerBaselineValid = false;
    g_powerBaseline[0] = 0;
}

static bool RunHiddenAndWait(const wchar_t* cmdLine, DWORD* exitCode)
{
    wchar_t buf[512];
    wcscpy(buf, cmdLine);
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode) *exitCode = rc;
    return true;
}

bool PowerApplyHighPerformance(void)
{
    wchar_t oldTxt[64];
    if (g_powerBaselineValid && g_powerBaseline[0]) {
        wcscpy(oldTxt, g_powerBaseline);         // 用守护启动时采集的基线
    } else {
        GUID* oldGuid = NULL;
        if (PowerGetActiveScheme(NULL, &oldGuid) != ERROR_SUCCESS || !oldGuid) {
            LogDiagWin(L"PowerGetActiveScheme");
            return false;
        }
        FormatGuidText(oldGuid, oldTxt, 64);
        LocalFree(oldGuid);
    }

    GUID hp = {0x8c5e7fda,0xe8bf,0x4a96,{0x9a,0x85,0xa6,0xe2,0x3a,0x8c,0x63,0x5c}};
    GUID up = {0xe9a4be1b,0x488e,0x4c60,{0x81,0x75,0x6b,0x7b,0x5f,0x9c,0x46,0x47}}; // 卓越性能
    DWORD rc = 1;

    // 优先卓越性能 (若模板存在)，否则高性能；缺失则 duplicatescheme 再试
    if (PowerSetActiveScheme(NULL, &up) == ERROR_SUCCESS) {
        LogAction(L"[+] 已激活 [卓越性能] 电源方案");
    } else if (PowerSetActiveScheme(NULL, &hp) == ERROR_SUCCESS) {
        LogAction(L"[+] 已激活 [高性能] 电源方案");
    } else {
        RunHiddenAndWait(L"powercfg -duplicatescheme 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c", &rc);
        if (PowerSetActiveScheme(NULL, &hp) == ERROR_SUCCESS) {
            LogAction(L"[+] 已复制并激活 [高性能] 电源方案");
        } else {
            LogAction(L"[-] 电源方案切换失败：请检查组策略是否禁用电源方案");
            return false;
        }
    }

    JournalEntry je; memset(&je, 0, sizeof(je));
    je.kind = JK_POWERPLAN;
    je.persistent = false;          // 会话项：游戏退出/关闭工具自动还原
    je.applied = true;
    wcscpy(je.key, L"power");
    wcscpy(je.name, L"电源方案");
    wcscpy(je.aux, oldTxt);
    JournalAdd(&je);
    return true;
}

// ---------------------------------------------------------------- FSO
static bool ReadAnsiFile(const wchar_t* path, char** outBuf, size_t* outLen);

bool GetCs2PathForCheckup(wchar_t* out, size_t cch)
{
    out[0] = 0;
    if (FindCs2Process(out, cch) != 0 && out[0]) return true;
    // 游戏未运行：从注册表 SteamPath + libraryfolders.vdf 解析安装位置
    wchar_t steam[MAX_PATH * 2] = L"";
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0,
                      KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD type = 0, cb = (DWORD)(sizeof(steam));
        RegQueryValueExW(hk, L"SteamPath", NULL, &type, (BYTE*)steam, &cb);
        RegCloseKey(hk);
    }
    if (!steam[0]) return false;
    for (wchar_t* p = steam; *p; p++) if (*p == L'/') *p = L'\\';
    size_t n = wcslen(steam);
    while (n > 1 && steam[n - 1] == L'\\') steam[--n] = 0;

    wchar_t probe[MAX_PATH * 2];
    swprintf(probe, cch,
             L"%s\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe",
             steam);
    if (FileExistsW(probe)) { wcsncpy(out, probe, cch - 1); out[cch - 1] = 0; return true; }

    // 多库：libraryfolders.vdf
    wchar_t libFile[MAX_PATH * 2];
    swprintf(libFile, (sizeof(libFile)/sizeof(wchar_t)),
             L"%s\\steamapps\\libraryfolders.vdf", steam);
    char* buf = NULL; size_t len = 0;
    bool found = false;
    if (ReadAnsiFile(libFile, &buf, &len)) {
        char* p = buf;
        while (p && !found) {
            char* q = strstr(p, "\"path\"");
            if (!q) break;
            q = strchr(q + 6, '"');
            if (!q) break;
            q++;
            char* e = strchr(q, '"');
            if (!e) break;
            char raw[1024];
            size_t cnt = (size_t)(e - q);
            if (cnt >= sizeof(raw)) cnt = sizeof(raw) - 1;
            memcpy(raw, q, cnt); raw[cnt] = 0;
            // 去 \\ 转义
            char* d = raw;
            for (char* s2 = raw; *s2; s2++) {
                if (s2[0] == '\\' && s2[1] == '\\') { *d++ = '\\'; s2++; }
                else *d++ = *s2;
            }
            *d = 0;
            wchar_t root[MAX_PATH * 2];
            U8ToW(raw, root, (int)(sizeof(root)/sizeof(wchar_t)));
            swprintf(probe, cch,
                     L"%s\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe",
                     root);
            if (FileExistsW(probe)) { wcsncpy(out, probe, cch - 1); out[cch - 1] = 0; found = true; }
            p = e;
        }
        free(buf);
    }
    return found;
}

void FsoDisableForCs2(void)
{
    wchar_t exe[MAX_PATH * 2] = L"";
    if (!GetCs2PathForCheckup(exe, (sizeof(exe)/sizeof(wchar_t)))) {
        LogAction(L"[-] 无法定位 cs2.exe：请启动游戏后重试，或确认 Steam 注册表完整。");
        return;
    }
    const wchar_t* sub = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_READ | KEY_WRITE, NULL, &hk, NULL) != ERROR_SUCCESS) {
        LogDiagWin(L"打开 AppCompat Layers");
        return;
    }
    wchar_t cur[1024] = L""; DWORD type = 0, cb = sizeof(cur);
    LONG q = RegQueryValueExW(hk, exe, NULL, &type, (BYTE*)cur, &cb);
    cur[1023] = 0;

    if (q == ERROR_SUCCESS && wcsstr(cur, L"DISABLEDXMAXIMIZEDWINDOWEDMODE")) {
        LogAction(L"[=] CS2 全屏优化此前已禁用。重启 CS2 后生效。");
        RegCloseKey(hk);
        return;
    }
    wchar_t next[1024];
    if (q == ERROR_SUCCESS && cur[0]) swprintf(next, 1024, L"%s ~ DISABLEDXMAXIMIZEDWINDOWEDMODE", cur);
    else swprintf(next, 1024, L"~ DISABLEDXMAXIMIZEDWINDOWEDMODE");

    if (RegSetValueExW(hk, exe, 0, REG_SZ, (const BYTE*)next,
                       (DWORD)((wcslen(next) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS) {
        JournalEntry je; memset(&je, 0, sizeof(je));
        je.kind = JK_FSO_LAYER;
        je.persistent = true;
        je.applied = true;
        wcsncpy(je.key, exe, 259);
        wcscpy(je.name, L"禁用全屏优化");
        if (q == ERROR_SUCCESS && cur[0]) wcscpy(je.oldStr, cur);
        else je.oldStr[0] = 1;                    // 原本不存在
        JournalAdd(&je);
        LogAction(L"[+] 已禁用 CS2 全屏优化 (AppCompat Layer)。重启 CS2 后生效。");
    } else {
        LogDiagWin(L"写入 AppCompat Layer");
    }
    RegCloseKey(hk);
}

// ---------------------------------------------------------------- HAGS
void HagsToggle(void)
{
    const wchar_t* gd = L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers";
    DWORD mode = 0;
    HKEY hk;
    bool known = false;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, gd, 0, KEY_READ | KEY_WRITE, &hk) == ERROR_SUCCESS) {
        DWORD type = 0, cb = sizeof(mode);
        known = (RegQueryValueExW(hk, L"HwSchMode", NULL, &type, (BYTE*)&mode, &cb)
                 == ERROR_SUCCESS && type == REG_DWORD);
        if (!known) mode = 1;
        DWORD target = (mode == 2) ? 1 : 2;
        DWORD old = mode;
        if (RegSetValueExW(hk, L"HwSchMode", 0, REG_DWORD,
                           (const BYTE*)&target, sizeof(target)) == ERROR_SUCCESS) {
            JournalEntry je; memset(&je, 0, sizeof(je));
            je.kind = JK_REG_DWORD;
            je.persistent = true;
            je.applied = true;
            wcscpy(je.key, L"HKLM|SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers|HwSchMode");
            wcscpy(je.name, L"硬件加速 GPU 调度 (HAGS)");
            je.oldDword = known ? old : 0xDEAD0001ull;
            je.newDword = target;
            JournalAdd(&je);
            LogAction(L"[+] HAGS 已设为 %s —— 需注销/重启生效。",
                      target == 2 ? L"开启" : L"关闭");
        } else {
            LogDiagWin(L"写入 HwSchMode (需要管理员)");
        }
        RegCloseKey(hk);
    } else {
        LogAction(L"[-] 打不开 GraphicsDrivers 键 (需要管理员权限)。");
    }
}

// ---------------------------------------------------------------- autoexec.cfg
static const char* kCfgLines[] = {
    "fps_max 0",
    "engine_low_latency_sleep_after_client_tick \"true\"",
    "r_show_build_info \"false\"",
    "cl_autohelp \"false\"",
    "cl_showhelp \"false\"",
    "r_drawtracers_firstperson \"false\"",
    NULL
};

static bool ReadAnsiFile(const wchar_t* path, char** outBuf, size_t* outLen)
{
    *outBuf = NULL; *outLen = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == 0xFFFFFFFF || sz > 2u * 1024 * 1024) { CloseHandle(h); return false; }
    char* buf = (char*)malloc(sz + 1);
    DWORD rd = 0;
    if (!buf || !ReadFile(h, buf, sz, &rd, NULL)) { free(buf); CloseHandle(h); return false; }
    buf[rd] = 0;
    CloseHandle(h);
    *outBuf = buf; *outLen = rd;
    return true;
}

static bool CfgHasCvar(const char* buf, size_t len, const char* name)
{
    size_t nl = strlen(name);
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && buf[i] != '\n') i++;
        size_t k = start;
        while (k < i && (buf[k] == ' ' || buf[k] == '\t')) k++;
        if (i - k >= nl && strncmp(buf + k, name, nl) == 0) {
            char nb = (k + nl < i) ? buf[k + nl] : '\0';
            if (nb == ' ' || nb == '\t' || nb == '\0') return true;
        }
        if (i < len) i++;
    }
    return false;
}

void CfgGenerateAutoexec(void)
{
    wchar_t local[MAX_PATH * 2];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local,
            (DWORD)(sizeof(local)/sizeof(wchar_t)))) {
        LogAction(L"[-] 无法读取 LOCALAPPDATA。");
        return;
    }
    wchar_t cfgDir[MAX_PATH * 2], cfgPath[MAX_PATH * 2];
    swprintf(cfgDir, (sizeof(cfgDir)/sizeof(wchar_t)),
             L"%s\\Counter-Strike Global Offensive\\game\\csgo\\cfg", local);
    swprintf(cfgPath, (sizeof(cfgPath)/sizeof(wchar_t)), L"%s\\autoexec.cfg", cfgDir);

    if (GetFileAttributesW(cfgDir) == INVALID_FILE_ATTRIBUTES) {
        EnsureDirTree(cfgDir);
        if (GetFileAttributesW(cfgDir) == INVALID_FILE_ATTRIBUTES) {
            LogDiagWin(L"创建 CS2 cfg 目录");
            return;
        }
    }

    char* have = NULL; size_t haveLen = 0;
    bool existed = FileExistsW(cfgPath);
    if (existed && !ReadAnsiFile(cfgPath, &have, &haveLen)) {
        LogAction(L"[-] 读取现有 autoexec.cfg 失败。");
        return;
    }
    if (existed) {
        wchar_t bak[MAX_PATH * 2];
        swprintf(bak, (sizeof(bak)/sizeof(wchar_t)), L"%s.bak", cfgPath);
        CopyFileW(cfgPath, bak, FALSE);
    }

    const char* missing[16]; int missN = 0;
    for (int i = 0; kCfgLines[i] && missN < 16; i++) {
        char name[64]; int n = 0;
        while (kCfgLines[i][n] && kCfgLines[i][n] != ' ' && n < 63) {
            name[n] = kCfgLines[i][n]; n++;
        }
        name[n] = 0;
        if (!have || !CfgHasCvar(have, haveLen, name)) missing[missN++] = kCfgLines[i];
    }
    free(have);

    if (missN == 0) {
        LogAction(L"[=] autoexec.cfg 已包含全部优化项。");
    } else {
        HANDLE h = CreateFileW(cfgPath, FILE_APPEND_DATA, 0, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { LogDiagWin(L"打开 autoexec.cfg"); return; }
        DWORD wr;
        const char* hdr = "\n// added by CS2Optimizer v2.0\n";
        if (existed) WriteFile(h, hdr, (DWORD)strlen(hdr), &wr, NULL);
        for (int i = 0; i < missN; i++) {
            WriteFile(h, missing[i], (DWORD)strlen(missing[i]), &wr, NULL);
            WriteFile(h, "\n", 1, &wr, NULL);
        }
        CloseHandle(h);
        LogAction(L"[+] 已写入 %d 项 cvar 到 autoexec.cfg", missN);
    }
    LogAction(L"[!] CS2 不自动执行 autoexec：请在启动项加  +exec autoexec.cfg");
}

// ---------------------------------------------------------------- 工作集整理
void PrewarmCs2WorkingSet(void)
{
    DWORD pid = FindCs2Process(NULL, 0);
    if (!pid) { LogAction(L"[-] CS2 未运行。"); return; }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA,
                           FALSE, pid);
    if (!h) { LogDiagWin(L"打开 CS2 (工作集)"); return; }
    // 提高工作集上限，减少换页抖动 (不做 EmptyWorkingSet —— 对游戏有害)
    SIZE_T minW = 64u << 20, maxW = 2u << 30;
    if (SetProcessWorkingSetSizeEx(h, minW, maxW,
            QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE))
        LogAction(L"[+] CS2 工作集下限已锁定 64MB (减少换页卡顿)");
    else
        LogDiagWin(L"SetProcessWorkingSetSizeEx");
    CloseHandle(h);
}
