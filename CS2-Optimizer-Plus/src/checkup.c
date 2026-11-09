// checkup.c — 系统体检 (Pavise 式: 采样 -> 证据分级 -> 一键修复)
// level: 0=已优化(绿) 1=提示(蓝) 2=需修复(橙红) 3=未知(灰)
#include "app.h"

static void AddFinding(Finding* out, int* n, int max, int level,
                       const wchar_t* text, int fixTweakIdx, int fixKind)
{
    if (*n >= max) return;
    out[*n].level = level;
    out[*n].fixTweakIdx = fixTweakIdx;
    out[*n].fixKind = fixKind;
    wcsncpy(out[*n].text, text, 199);
    out[*n].text[199] = 0;
    (*n)++;
}

static bool RegReadDw(HKEY root, const wchar_t* sub, const wchar_t* val, DWORD* out)
{
    HKEY hk;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    DWORD type = 0, cb = sizeof(*out);
    LONG r = RegQueryValueExW(hk, val, NULL, &type, (BYTE*)out, &cb);
    RegCloseKey(hk);
    return r == ERROR_SUCCESS && type == REG_DWORD;
}

int CheckupRun(Finding* out, int max)
{
    int n = 0;
    int tn; const RegTweak* table = TweakTable(&tn);
    wchar_t buf[200];

    // 1) 注册表优化项逐条
    for (int i = 0; i < tn; i++) {
        if (table[i].needsAdmin && !IsRunAsAdmin()) {
            _snwprintf(buf, 200, L"%s：权限不足，无法检查", table[i].label);
            AddFinding(out, &n, max, 3, buf, i, 1);
            continue;
        }
        bool ok = TweakIsApplied(&table[i]);
        _snwprintf(buf, 200, L"%s：%s", table[i].label,
                   ok ? L"已优化" : L"未优化");
        AddFinding(out, &n, max, ok ? 0 : 2, buf, i, ok ? 0 : 1);
    }

    // 2) 电源方案
    {
        GUID* act = NULL;
        if (PowerGetActiveScheme(NULL, &act) == ERROR_SUCCESS && act) {
            GUID hp = {0x8c5e7fda,0xe8bf,0x4a96,{0x9a,0x85,0xa6,0xe2,0x3a,0x8c,0x63,0x5c}};
            GUID up = {0xe9a4be1b,0x488e,0x4c60,{0x81,0x75,0x6b,0x7b,0x5f,0x9c,0x46,0x47}};
            bool perf = (memcmp(act, &hp, sizeof(GUID)) == 0 ||
                         memcmp(act, &up, sizeof(GUID)) == 0);
            AddFinding(out, &n, max, perf ? 0 : 2,
                       perf ? L"电源方案：高性能/卓越性能" : L"电源方案：平衡 (建议切换高性能)",
                       -1, perf ? 0 : 2);
            LocalFree(act);
        } else {
            AddFinding(out, &n, max, 3, L"电源方案：读取失败", -1, 0);
        }
    }

    // 3) HAGS
    {
        DWORD hags = 0;
        if (RegReadDw(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                L"HwSchMode", &hags)) {
            bool on = (hags == 2);
            _snwprintf(buf, 200, L"硬件加速 GPU 调度 (HAGS)：%s",
                       on ? L"开启" : L"关闭 (Win11 + 新驱动建议开启)");
            AddFinding(out, &n, max, on ? 0 : 1, buf, -1, on ? 0 : 4);
        } else {
            AddFinding(out, &n, max, 3, L"HAGS：驱动未暴露状态键", -1, 0);
        }
    }

    // 4) VBS / 内存完整性 (Win11 帧率杀手之一)
    {
        DWORD ev1 = 0, ev2 = 0;
        bool has = RegReadDw(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
            L"EnableVirtualizationBasedSecurity", &ev1);
        bool has2 = RegReadDw(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
            L"Enabled", &ev2);
        if (has && ev1 == 0) {
            AddFinding(out, &n, max, 0, L"VBS：未启用 (对性能友好)", -1, 0);
        } else if (has) {
            _snwprintf(buf, 200, L"VBS：已启用%s —— 可能损失 5-10%% 游戏性能，"
                       L"可在 Windows 安全中心关闭内核隔离 (需重启)",
                       (has2 && ev2 == 1) ? L" (含内存完整性)" : L"");
            AddFinding(out, &n, max, 1, buf, -1, 0);
        } else {
            AddFinding(out, &n, max, 3, L"VBS：无法读取 (需要管理员)", -1, 0);
        }
    }

    // 5) FSO (CS2 路径的 AppCompat Layer)
    {
        wchar_t exe[MAX_PATH * 2] = L"";
        if (GetCs2PathForCheckup(exe, (sizeof(exe)/sizeof(wchar_t)))) {
            HKEY hk;
            bool disabled = false;
            if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                    0, KEY_READ, &hk) == ERROR_SUCCESS) {
                wchar_t cur[1024] = L""; DWORD type = 0, cb = sizeof(cur);
                if (RegQueryValueExW(hk, exe, NULL, &type, (BYTE*)cur, &cb) == ERROR_SUCCESS)
                    disabled = wcsstr(cur, L"DISABLEDXMAXIMIZEDWINDOWEDMODE") != NULL;
                RegCloseKey(hk);
            }
            _snwprintf(buf, 200, L"全屏优化 (FSO)：%s",
                       disabled ? L"已禁用" : L"仍启用 (禁用可降输入延迟)");
            AddFinding(out, &n, max, disabled ? 0 : 2, buf, -1, disabled ? 0 : 3);
        } else {
            AddFinding(out, &n, max, 3, L"FSO：未定位到 cs2.exe，跳过", -1, 0);
        }
    }

    // 6) autoexec.cfg
    {
        wchar_t local[MAX_PATH * 2];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local,
                (DWORD)(sizeof(local)/sizeof(wchar_t)))) {
            wchar_t p[MAX_PATH * 2];
            swprintf(p, (sizeof(p)/sizeof(wchar_t)),
                     L"%s\\Counter-Strike Global Offensive\\game\\csgo\\cfg\\autoexec.cfg", local);
            bool exists = FileExistsW(p);
            _snwprintf(buf, 200, L"autoexec.cfg：%s",
                       exists ? L"已生成 (记得 +exec autoexec.cfg)" : L"尚未生成");
            AddFinding(out, &n, max, exists ? 0 : 2, buf, -1, exists ? 0 : 5);
        }
    }

    // 7) 游戏模式 / Game Bar
    {
        DWORD gm = 0;
        if (RegReadDw(HKEY_CURRENT_USER,
                L"SOFTWARE\\Microsoft\\GameBar", L"AllowAutoGameMode", &gm)) {
            _snwprintf(buf, 200, L"Windows 游戏模式：%s", gm ? L"开启" : L"关闭");
            AddFinding(out, &n, max, 1, buf, -1, 0);
        }
    }

    // 8) 内存容量提示
    {
        MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            DWORD gb = (DWORD)(ms.ullTotalPhys >> 30);
            _snwprintf(buf, 200, L"物理内存：%lu GB (可用 %lu GB)%s",
                       gb, (DWORD)(ms.ullAvailPhys >> 30),
                       gb < 16 ? L" —— CS2 建议 16GB+" : L"");
            AddFinding(out, &n, max, gb < 16 ? 1 : 0, buf, -1, 0);
        }
    }

    LogDiag(L"体检完成：%d 条结论", n);
    return n;
}

void CheckupFixAll(const Finding* f, int n)
{
    int tn; const RegTweak* table = TweakTable(&tn);
    int fixed = 0;
    for (int i = 0; i < n; i++) {
        if (f[i].level != 2) continue;
        switch (f[i].fixKind) {
        case 1:
            if (f[i].fixTweakIdx >= 0 && f[i].fixTweakIdx < tn) {
                const RegTweak* t = &table[f[i].fixTweakIdx];
                if (t->needsAdmin && !IsRunAsAdmin()) continue;
                if (TweakApply(t)) fixed++;
            }
            break;
        case 2: if (PowerApplyHighPerformance()) fixed++; break;
        case 3: FsoDisableForCs2(); fixed++; break;
        case 4: HagsToggle(); fixed++; break;
        case 5: CfgGenerateAutoexec(); fixed++; break;
        default: break;
        }
    }
    LogAction(L"一键修复完成：%d 项", fixed);
}
