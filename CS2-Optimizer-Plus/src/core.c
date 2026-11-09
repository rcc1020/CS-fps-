// core.c — 目录 / 双日志 / 原子文件 / 会话事务 journal / 白名单 / 设置 / CPU 拓扑
#include "app.h"
#include <stddef.h>

#define ACTION_LOG_MAX   2000
#define DIAG_LOG_MAX     4000
#define GUI_QUEUE_SLOTS  512
#define GUI_LINE_LIMIT   512

// ------------------------------------------------------------------ 目录
static wchar_t g_dirApp[MAX_PATH * 2];
static wchar_t g_dirLog[MAX_PATH * 2];
static wchar_t g_pathJournal[MAX_PATH * 2];
static wchar_t g_pathWhite[MAX_PATH * 2];
static wchar_t g_pathSet[MAX_PATH * 2];

void EnsureDirTree(const wchar_t* path)
{
    wchar_t buf[MAX_PATH * 2];
    wcscpy(buf, path);
    for (wchar_t* p = buf + 1; *p; p++) {
        if (*p == L'\\') { *p = 0; CreateDirectoryW(buf, NULL); *p = L'\\'; }
    }
    CreateDirectoryW(buf, NULL);
}

void AppDirsInit(void)
{
    wchar_t appdata[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH))
        wcscpy(appdata, L".");
    swprintf(g_dirApp, (sizeof(g_dirApp)/sizeof(wchar_t)), L"%s\\CS2Optimizer", appdata);
    swprintf(g_dirLog, (sizeof(g_dirLog)/sizeof(wchar_t)), L"%s\\logs", g_dirApp);
    EnsureDirTree(g_dirApp);
    EnsureDirTree(g_dirLog);
    swprintf(g_pathJournal, (sizeof(g_pathJournal)/sizeof(wchar_t)), L"%s\\journal.txt", g_dirApp);
    swprintf(g_pathWhite,   (sizeof(g_pathWhite)/sizeof(wchar_t)),   L"%s\\whitelist.txt", g_dirApp);
    swprintf(g_pathSet,     (sizeof(g_pathSet)/sizeof(wchar_t)),     L"%s\\settings.ini", g_dirApp);
}
const wchar_t* AppDir(void)          { return g_dirApp; }
const wchar_t* AppLogDir(void)       { return g_dirLog; }
const wchar_t* AppJournalPath(void)  { return g_pathJournal; }
const wchar_t* AppWhitelistPath(void){ return g_pathWhite; }
const wchar_t* AppSettingsPath(void) { return g_pathSet; }

bool FileExistsW(const wchar_t* path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void U8ToW(const char* u8, wchar_t* out, int cch)
{
    if (cch <= 0) return;
    out[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, cch);
    out[cch - 1] = 0;
}
void WToU8(const wchar_t* w, char* out, int cb)
{
    if (cb <= 0) return;
    out[0] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cb, NULL, NULL);
    out[cb - 1] = 0;
}

// 读文本文件 (UTF-8 -> wchar_t)，调用方 free()
void ReadFileText(const wchar_t* path, wchar_t** outBuf, size_t* outLen)
{
    *outBuf = NULL; if (outLen) *outLen = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == 0 || sz > 8u * 1024 * 1024) { CloseHandle(h); return; }
    char* raw = (char*)malloc(sz + 4);
    if (!raw) { CloseHandle(h); return; }
    DWORD rd = 0;
    if (!ReadFile(h, raw, sz, &rd, NULL) || rd == 0) { free(raw); CloseHandle(h); return; }
    raw[rd] = 0;
    CloseHandle(h);
    int wc = MultiByteToWideChar(CP_UTF8, 0, raw, (int)rd, NULL, 0);
    wchar_t* w = (wchar_t*)malloc(((size_t)wc + 1) * sizeof(wchar_t));
    if (w) {
        MultiByteToWideChar(CP_UTF8, 0, raw, (int)rd, w, wc);
        w[wc] = 0;
        *outBuf = w;
        if (outLen) *outLen = (size_t)wc;
    }
    free(raw);
}

// 原子写：temp + ReplaceFile/MoveFileEx，防半截文件
bool WriteFileAtomic(const wchar_t* path, const wchar_t* text)
{
    wchar_t tmp[MAX_PATH * 2];
    swprintf(tmp, (sizeof(tmp)/sizeof(wchar_t)), L"%s.tmp", path);
    int cb = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (cb <= 0) return false;
    char* u8 = (char*)malloc((size_t)cb);
    if (!u8) return false;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, u8, cb, NULL, NULL);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = false;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        ok = WriteFile(h, u8, (DWORD)cb, &wr, NULL) != 0;
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    free(u8);
    if (ok) {
        ok = ReplaceFileW(path, tmp, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS,
                          NULL, NULL) != 0;
        if (!ok) ok = MoveFileExW(tmp, path,
                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) DeleteFileW(tmp);
    } else {
        DeleteFileW(tmp);
    }
    return ok;
}

// 跨编译器分词 (MinGW wcstok 是 3 参安全版)
static wchar_t* TokW(wchar_t* str, const wchar_t* delim, wchar_t** save)
{
#ifdef _MSC_VER
    return wcstok(str, delim, save);
#else
    return wcstok(str, delim, save);
#endif
}
// ------------------------------------------------------------------ 双日志
static CRITICAL_SECTION g_logCs;
static wchar_t g_actionBuf[ACTION_LOG_MAX][300];
static int     g_actionN, g_actionWritten;
static wchar_t g_diagBuf[DIAG_LOG_MAX][300];
static int     g_diagN, g_diagWritten;
static wchar_t g_guiQueue[2][GUI_QUEUE_SLOTS][GUI_LINE_LIMIT];
static volatile LONG g_guiHead[2], g_guiTail[2];
static HWND    g_guiMainWnd;
static bool    g_dirty;

void GuiLogSetWnd(HWND hwnd) { g_guiMainWnd = hwnd; }

static void LogStamp(wchar_t* out, size_t cch)
{
    SYSTEMTIME st; GetLocalTime(&st);
    swprintf(out, cch, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
}

static void LogAppendBuf(wchar_t buf[][300], int* n, int max, const wchar_t* line)
{
    if (*n >= max) {
        int keep = max / 2;
        memmove(buf, buf + (max - keep), (size_t)keep * 300 * sizeof(wchar_t));
        *n = keep;
    }
    wcsncpy(buf[*n], line, 299);
    buf[*n][299] = 0;
    (*n)++;
    g_dirty = true;
}

static void GuiQueuePush(int q, const wchar_t* line)
{
    EnterCriticalSection(&g_logCs);
    LONG next = (g_guiTail[q] + 1) % GUI_QUEUE_SLOTS;
    if (next != g_guiHead[q]) {
        wcsncpy(g_guiQueue[q][g_guiTail[q]], line, GUI_LINE_LIMIT - 1);
        g_guiQueue[q][g_guiTail[q]][GUI_LINE_LIMIT - 1] = 0;
        g_guiTail[q] = next;
    }
    LeaveCriticalSection(&g_logCs);
    if (g_guiMainWnd) PostMessageW(g_guiMainWnd, WM_APP_LOGQ, 0, 0);
}

void LogInit(void)
{
    InitializeCriticalSection(&g_logCs);
    g_guiHead[0] = g_guiTail[0] = g_guiHead[1] = g_guiTail[1] = 0;
    g_actionN = g_diagN = g_actionWritten = g_diagWritten = 0;
    g_guiMainWnd = NULL;
    g_dirty = false;
}

void LogAction(const wchar_t* fmt, ...)
{
    wchar_t msg[280], line[300], ts[16];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(msg, 279, fmt, ap); msg[279] = 0; va_end(ap);
    LogStamp(ts, 16);
    _snwprintf(line, 300, L"%s%s", ts, msg); line[299] = 0;

    EnterCriticalSection(&g_logCs);
    LogAppendBuf(g_actionBuf, &g_actionN, ACTION_LOG_MAX, line);
    LeaveCriticalSection(&g_logCs);
    GuiQueuePush(0, line);
}

void LogDiag(const wchar_t* fmt, ...)
{
    wchar_t msg[280], line[300], ts[16];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(msg, 279, fmt, ap); msg[279] = 0; va_end(ap);
    LogStamp(ts, 16);
    _snwprintf(line, 300, L"%s%s", ts, msg); line[299] = 0;
    EnterCriticalSection(&g_logCs);
    LogAppendBuf(g_diagBuf, &g_diagN, DIAG_LOG_MAX, line);
    LeaveCriticalSection(&g_logCs);
    GuiQueuePush(1, line);
}

void LogDiagWin(const wchar_t* what)
{
    DWORD e = GetLastError();
    LPWSTR sys = NULL;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0,
                       (LPWSTR)&sys, 0, NULL) && sys) {
        LogDiag(L"%s: err=%lu %s", what, e, sys);
        LocalFree(sys);
    } else LogDiag(L"%s: err=%lu", what, e);
}

static void DrainOneQueue(int q, HWND hEdit)
{
    for (;;) {
        wchar_t line[GUI_LINE_LIMIT];
        EnterCriticalSection(&g_logCs);
        if (g_guiHead[q] == g_guiTail[q]) { LeaveCriticalSection(&g_logCs); break; }
        wcscpy(line, g_guiQueue[q][g_guiHead[q]]);
        g_guiHead[q] = (g_guiHead[q] + 1) % GUI_QUEUE_SLOTS;
        LeaveCriticalSection(&g_logCs);
        if (!hEdit) continue;
        int len = GetWindowTextLengthW(hEdit);
        SendMessageW(hEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(hEdit, EM_REPLACESEL, FALSE, (LPARAM)line);
        SendMessageW(hEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    }
    if (hEdit) SendMessageW(hEdit, WM_VSCROLL, SB_BOTTOM, 0);
}

void GuiLogDrain(HWND hAction, HWND hDiag)
{
    DrainOneQueue(0, hAction);
    DrainOneQueue(1, hDiag);
}

int LogLineCount(int q)
{
    EnterCriticalSection(&g_logCs);
    int n = (q == 0) ? g_actionN : g_diagN;
    LeaveCriticalSection(&g_logCs);
    return n;
}

bool LogGetLine(int q, int idx, wchar_t* out, int cch)
{
    if (!out || cch <= 0) return false;
    out[0] = 0;
    bool ok = false;
    EnterCriticalSection(&g_logCs);
    int n = (q == 0) ? g_actionN : g_diagN;
    if (idx >= 0 && idx < n) {
        const wchar_t* src = (q == 0) ? g_actionBuf[idx] : g_diagBuf[idx];
        wcsncpy(out, src, (size_t)cch - 1);
        out[cch - 1] = 0;
        ok = true;
    }
    LeaveCriticalSection(&g_logCs);
    return ok;
}

void LogClearMemory(int q)
{
    EnterCriticalSection(&g_logCs);
    if (q == 0) { g_actionN = 0; g_actionWritten = 0; }
    else { g_diagN = 0; g_diagWritten = 0; }
    LeaveCriticalSection(&g_logCs);
}

void LogFlushToFiles(void)
{
    EnterCriticalSection(&g_logCs);
    if (!g_dirty) { LeaveCriticalSection(&g_logCs); return; }
    g_dirty = false;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t path[MAX_PATH * 2];
    swprintf(path, (sizeof(path)/sizeof(wchar_t)),
             L"%s\\session-%04d%02d%02d.log", g_dirLog, st.wYear, st.wMonth, st.wDay);

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (g_actionN < g_actionWritten) g_actionWritten = 0;   // 环形重置 -> 全量
        for (int i = g_actionWritten; i < g_actionN; i++) {
            char u8[1200]; WToU8(g_actionBuf[i], u8, sizeof(u8));
            DWORD wr;
            WriteFile(h, u8, (DWORD)strlen(u8), &wr, NULL);
            WriteFile(h, "\r\n", 2, &wr, NULL);
        }
        g_actionWritten = g_actionN;
        CloseHandle(h);
    }

    // 诊断日志单独落盘: 之前只写操作缓冲, 导致 diagnostics 全部丢失
    swprintf(path, (sizeof(path)/sizeof(wchar_t)),
             L"%s\\diagnostic-%04d%02d%02d.log", g_dirLog, st.wYear, st.wMonth, st.wDay);
    h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (g_diagN < g_diagWritten) g_diagWritten = 0;
        for (int i = g_diagWritten; i < g_diagN; i++) {
            char u8[1200]; WToU8(g_diagBuf[i], u8, sizeof(u8));
            DWORD wr;
            WriteFile(h, u8, (DWORD)strlen(u8), &wr, NULL);
            WriteFile(h, "\r\n", 2, &wr, NULL);
        }
        g_diagWritten = g_diagN;
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logCs);
}

// GUID 文本 <-> 结构 (不依赖 ole32 的 CLSIDFromString)
static int HexVal(wchar_t c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}
static bool HexBytes(const wchar_t* s, int n, uint64_t* out)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        int h = HexVal(s[i]);
        if (h < 0) return false;
        v = (v << 4) | (uint64_t)h;
    }
    *out = v;
    return true;
}

bool ParseGuidText(const wchar_t* s, GUID* out)
{
    if (!s || !out) return false;
    if (*s == L'{') s++;
    uint64_t a, b, c, d1, d2;
    // Data1/2/3 + Data4[0..1] 为 8-4-4-4; 末段 12 位十六进制必须用 64 位装,
    // 否则 32 位 unsigned long 会截断高 4 位 -> 解析出错误 GUID。
    if (!HexBytes(s, 8, &a)) return false;
    if (s[8] != L'-') return false;
    if (!HexBytes(s + 9, 4, &b)) return false;
    if (s[13] != L'-') return false;
    if (!HexBytes(s + 14, 4, &c)) return false;
    if (s[18] != L'-') return false;
    if (!HexBytes(s + 19, 4, &d1)) return false;
    if (s[23] != L'-') return false;
    if (!HexBytes(s + 24, 12, &d2)) return false;
    out->Data1 = (DWORD)a;
    out->Data2 = (WORD)b;
    out->Data3 = (WORD)c;
    out->Data4[0] = (BYTE)(d1 >> 8);
    out->Data4[1] = (BYTE)(d1);
    for (int i = 0; i < 6; i++)
        out->Data4[2 + i] = (BYTE)(d2 >> ((5 - i) * 8));
    return true;
}

void FormatGuidText(const GUID* g, wchar_t* out, size_t cch)
{
    static const wchar_t hx[] = L"0123456789abcdef";
    size_t i = 0;
    DWORD v = g->Data1;
    for (int sh = 28; sh >= 0; sh -= 4) out[i++] = hx[(v >> sh) & 0xF];
    out[i++] = L'-';
    WORD w = g->Data2;
    for (int sh = 12; sh >= 0; sh -= 4) out[i++] = hx[(w >> sh) & 0xF];
    out[i++] = L'-';
    w = g->Data3;
    for (int sh = 12; sh >= 0; sh -= 4) out[i++] = hx[(w >> sh) & 0xF];
    out[i++] = L'-';
    for (int k = 0; k < 2; k++) {
        out[i++] = hx[(g->Data4[k] >> 4) & 0xF];
        out[i++] = hx[g->Data4[k] & 0xF];
    }
    out[i++] = L'-';
    for (int k = 2; k < 8; k++) {
        out[i++] = hx[(g->Data4[k] >> 4) & 0xF];
        out[i++] = hx[g->Data4[k] & 0xF];
    }
    out[i] = 0;
    (void)cch;
}

// ------------------------------------------------------------------ 权限
bool IsRunAsAdmin(void)
{
    BOOL isAdmin = FALSE;
    PSID pAdmin = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &pAdmin)) {
        CheckTokenMembership(NULL, pAdmin, &isAdmin);
        FreeSid(pAdmin);
    }
    return isAdmin == TRUE;
}

bool RelaunchElevated(int autoClickId)
{
    wchar_t exe[MAX_PATH * 2];
    if (!GetModuleFileNameW(NULL, exe, (DWORD)(sizeof(exe)/sizeof(wchar_t))))
        return false;
    wchar_t params[64] = L"";
    if (autoClickId > 0) swprintf(params, 64, L"--autoclick=%d", autoClickId);
    HINSTANCE r = ShellExecuteW(NULL, L"runas", exe, params, NULL, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

// ------------------------------------------------------------------ journal
// key 格式: "HKCU|Software\\...|ValueName" (注册表) / exe 全路径 (FSO) / "pid"
JournalEntry g_journal[JOURNAL_MAX];
int          g_journalCount;
volatile LONG g_suppressedCount, g_restoredCount;

static void JournalRewriteDisk(void)
{
    size_t cap = 96u * 1024;
    wchar_t* s = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if (!s) return;
    size_t u = 0;
    u += (size_t)swprintf(s + u, cap - u, L"%s\n", JOURNAL_MAGIC);
    for (int i = 0; i < g_journalCount && u + 800 < cap; i++) {
        JournalEntry* j = &g_journal[i];
        u += (size_t)swprintf(s + u, cap - u,
            L"%d\t%d\t%d\t%s\t%s\t%llu\t%llu\t%lu\t%llu\t%s\t%s\n",
            (int)j->kind, j->persistent ? 1 : 0, j->applied ? 1 : 0,
            j->key, j->name,
            (unsigned long long)j->oldDword, (unsigned long long)j->newDword,
            j->pid, (unsigned long long)j->createTime,
            j->oldStr, j->aux);
    }
    WriteFileAtomic(g_pathJournal, s);
    free(s);
}

void JournalSaveToDisk(void) { JournalRewriteDisk(); }

void JournalClearDisk(void)
{
    g_journalCount = 0;
    DeleteFileW(g_pathJournal);
}

void JournalAdd(const JournalEntry* e)
{
    if (g_journalCount >= JOURNAL_MAX) { LogDiag(L"journal 已满"); return; }
    g_journal[g_journalCount] = *e;
    g_journalCount++;
    JournalRewriteDisk();
}

bool JournalFindByKey(JournalKind kind, const wchar_t* key, const wchar_t* name,
                      JournalEntry** out)
{
    for (int i = 0; i < g_journalCount; i++) {
        JournalEntry* j = &g_journal[i];
        if (j->kind == kind && _wcsicmp(j->key, key) == 0 &&
            (!name || !*name || _wcsicmp(j->name, name) == 0)) {
            if (out) *out = j;
            return true;
        }
    }
    return false;
}

static void JournalRemoveAt(int idx)
{
    if (idx < 0 || idx >= g_journalCount) return;
    memmove(&g_journal[idx], &g_journal[idx + 1],
            (size_t)(g_journalCount - idx - 1) * sizeof(JournalEntry));
    g_journalCount--;
}

// key "HKCU|Sub\\Path|Value" -> root / sub / val
static bool SplitRegKey(const wchar_t* key, HKEY* root, wchar_t* sub, size_t cchSub,
                        wchar_t* val, size_t cchVal)
{
    const wchar_t* p1 = wcschr(key, L'|');
    if (!p1) return false;
    const wchar_t* p2 = wcschr(p1 + 1, L'|');
    if (!p2) return false;
    *root = (wcsncmp(key, L"HKLM", 4) == 0) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    size_t n = (size_t)(p2 - p1 - 1);
    if (n >= cchSub) n = cchSub - 1;
    wcsncpy(sub, p1 + 1, n); sub[n] = 0;
    n = wcslen(p2 + 1);
    if (n >= cchVal) n = cchVal - 1;
    wcsncpy(val, p2 + 1, n); val[n] = 0;
    return true;
}

static void JournalRestoreOne(JournalEntry* j)
{
    switch (j->kind) {
    case JK_REG_DWORD: {
        HKEY root; wchar_t sub[260], val[80];
        if (!SplitRegKey(j->key, &root, sub, 260, val, 80)) break;
        HKEY hk;
        if (RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
            if (j->oldDword == (uint64_t)0xDEAD0001ull)
                RegDeleteValueW(hk, val);
            else {
                DWORD v = (DWORD)j->oldDword;
                RegSetValueExW(hk, val, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
            }
            RegCloseKey(hk);
            LogAction(L"[还原] %s", j->name);
        } else {
            LogDiagWin(L"journal 还原打开注册表");
        }
        break;
    }
    case JK_REG_SZ:
    case JK_FSO_LAYER: {
        HKEY root; wchar_t sub[260], val[80];
        if (j->kind == JK_FSO_LAYER) {
            root = HKEY_CURRENT_USER;
            wcscpy(sub, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers");
            wcsncpy(val, j->key, 79); val[79] = 0;
        } else if (!SplitRegKey(j->key, &root, sub, 260, val, 80)) break;
        HKEY hk;
        if (RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
            if (j->oldStr[0] == 1) RegDeleteValueW(hk, val);
            else RegSetValueExW(hk, val, 0, REG_SZ, (const BYTE*)j->oldStr,
                                (DWORD)((wcslen(j->oldStr) + 1) * sizeof(wchar_t)));
            RegCloseKey(hk);
            LogAction(L"[还原] 全屏优化登记已清除");
        }
        break;
    }
    case JK_POWERPLAN: {
        GUID g2;
        if (ParseGuidText(j->aux, &g2)) {
            if (PowerSetActiveScheme(NULL, &g2) == ERROR_SUCCESS) {
                // 回读校验: ProcessLasso/Bitsum 等外部电源管理会在游戏进程起停时抢先改方案,
                // 只报告"设置成功"会掩盖实际没还原到基线的情况。
                GUID* now = NULL;
                if (PowerGetActiveScheme(NULL, &now) == ERROR_SUCCESS && now) {
                    if (memcmp(now, &g2, sizeof(GUID)) == 0) {
                        LogAction(L"[还原] 电源方案已恢复");
                    } else {
                        wchar_t a[64], b[64];
                        FormatGuidText(&g2, a, 64);
                        FormatGuidText(now, b, 64);
                        LogAction(L"[!] 电源方案被外部程序改写：期望 %s，实际 %s", a, b);
                        LogAction(L"[!] 检测到外部电源管理 (如 ProcessLasso/Bitsum)，可能与本工具争抢方案。");
                    }
                    LocalFree(now);
                } else {
                    LogAction(L"[还原] 电源方案已恢复");
                }
            } else {
                LogDiagWin(L"PowerSetActiveScheme 还原");
            }
        } else {
            LogDiag(L"journal 电源 GUID 无效: %s", j->aux);
        }
        break;
    }
    case JK_PRIORITY:
    case JK_AFFINITY:
    case JK_ECOQOS:
    case JK_MEMPRIORITY: {
        if (j->pid == 0) break;
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, j->pid);
        if (!h) break;                       // 进程已退出 = 无需还原
        FILETIME c, e, k, u;
        if (GetProcessTimes(h, &c, &e, &k, &u)) {
            ULARGE_INTEGER ul; ul.LowPart = c.dwLowDateTime; ul.HighPart = c.dwHighDateTime;
            if (j->createTime && ul.QuadPart != j->createTime) {
                LogDiag(L"PID %lu 已复用，跳过还原", j->pid);
                CloseHandle(h); break;
            }
        }
        if (j->kind == JK_PRIORITY) {
            SetPriorityClass(h, (DWORD)j->oldDword);
            InterlockedIncrement(&g_restoredCount);
        } else if (j->kind == JK_AFFINITY) {
            SetProcessAffinityMask(h, (DWORD_PTR)j->oldDword);
        } else if (j->kind == JK_ECOQOS) {
            PROCESS_POWER_THROTTLING_STATE t;
            memset(&t, 0, sizeof(t));
            t.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            t.ControlMask = 0; t.StateMask = 0;
            SetProcessInformation(h, ProcessPowerThrottling, &t, sizeof(t));
        } else if (j->kind == JK_MEMPRIORITY) {
            MEMORY_PRIORITY_INFORMATION m;
            m.MemoryPriority = (DWORD)j->oldDword;
            SetProcessInformation(h, ProcessMemoryPriority, &m, sizeof(m));
        }
        CloseHandle(h);
        break;
    }
    default: break;
    }
    j->applied = false;
}

bool JournalRevertRegKey(const wchar_t* key)
{
    for (int i = 0; i < g_journalCount; i++) {
        JournalEntry* j = &g_journal[i];
        if ((j->kind == JK_REG_DWORD || j->kind == JK_REG_SZ) &&
            _wcsicmp(j->key, key) == 0) {
            JournalRestoreOne(j);
            JournalRemoveAt(i);
            JournalRewriteDisk();
            return true;
        }
    }
    return false;
}

void JournalRestore(bool persistentOnly, bool sessionOnly, const wchar_t* reason)
{
    if (g_journalCount == 0) {
        if (reason && *reason) LogAction(L"还原(%s)：无待还原项", reason);
        return;
    }
    if (reason && *reason) LogAction(L"—— 开始还原：%s ——", reason);
    int before = g_journalCount;
    for (int i = 0; i < g_journalCount; ) {
        JournalEntry* j = &g_journal[i];
        bool want = (persistentOnly && j->persistent) ||
                    (sessionOnly && !j->persistent) ||
                    (!persistentOnly && !sessionOnly);
        if (want) {
            JournalRestoreOne(j);
            JournalRemoveAt(i);
        } else i++;
    }
    LogAction(L"还原完成：本次处理 %d 项，剩余 %d 项", before - g_journalCount, g_journalCount);
    JournalRewriteDisk();
}

void JournalLoadAtBoot(void)
{
    wchar_t* buf = NULL; size_t len = 0;
    ReadFileText(g_pathJournal, &buf, &len);
    if (!buf) return;
    if (wcsncmp(buf, JOURNAL_MAGIC, wcslen(JOURNAL_MAGIC)) != 0) {
        LogDiag(L"journal 魔数不符，忽略");
        free(buf); return;
    }
    wchar_t* lsave = NULL; wchar_t* line = TokW(buf, L"\n", &lsave);
    while (line && g_journalCount < JOURNAL_MAX) {
        wchar_t* fields[11] = {0};
        int f = 0;
        fields[0] = line;
        for (wchar_t* p = line; *p && f < 10; p++)
            if (*p == L'\t') { *p = 0; fields[++f] = p + 1; }
        JournalEntry j; memset(&j, 0, sizeof(j));
        if (fields[0]) j.kind = (JournalKind)_wtoi(fields[0]);
        if (fields[1]) j.persistent = _wtoi(fields[1]) != 0;
        if (fields[2]) j.applied = _wtoi(fields[2]) != 0;
        if (fields[3]) wcsncpy(j.key, fields[3], 259);
        if (fields[4]) wcsncpy(j.name, fields[4], 79);
        if (fields[5]) j.oldDword = _wcstoui64(fields[5], NULL, 10);
        if (fields[6]) j.newDword = _wcstoui64(fields[6], NULL, 10);
        if (fields[7]) j.pid = (DWORD)_wtoi(fields[7]);
        if (fields[8]) j.createTime = _wcstoui64(fields[8], NULL, 10);
        if (fields[9]) wcsncpy(j.oldStr, fields[9], 259);
        if (fields[10]) wcsncpy(j.aux, fields[10], 79);
        if (j.applied) g_journal[g_journalCount++] = j;
        line = TokW(NULL, L"\n", &lsave);
    }
    free(buf);
    if (g_journalCount > 0) {
        LogAction(L"[!] 检测到上次会话有 %d 项未还原 (异常退出/重启)，正在恢复…", g_journalCount);
        JournalRestore(false, false, L"崩溃恢复");
    }
}

// ------------------------------------------------------------------ 白名单
static wchar_t g_whitelist[128][80];
static int     g_whitelistN;

static const wchar_t* kProtectedDefaults[] = {
    L"cs2.exe", L"steam.exe", L"steamwebhelper.exe", L"steamservice.exe",
    L"gameoverlayui.exe", L"battleye.exe", L"beclient.exe", L"easyanticheat.exe",
    NULL
};

static void WhitelistPersist(void)
{
    wchar_t s[128 * 84];
    size_t u = 0;
    for (int i = 0; i < g_whitelistN; i++)
        u += (size_t)swprintf(s + u, (sizeof(s)/sizeof(wchar_t)) - u, L"%s\n",
                              g_whitelist[i]);
    WriteFileAtomic(g_pathWhite, s);
}

void WhitelistInit(void)
{
    g_whitelistN = 0;
    wchar_t* buf = NULL; size_t len = 0;
    ReadFileText(g_pathWhite, &buf, &len);
    if (buf) {
        wchar_t* toksave = NULL; wchar_t* tok = TokW(buf, L"\r\n", &toksave);
        while (tok && g_whitelistN < 128) {
            if (*tok) {
                wcsncpy(g_whitelist[g_whitelistN], tok, 79);
                g_whitelist[g_whitelistN][79] = 0;
                g_whitelistN++;
            }
            tok = TokW(NULL, L"\r\n", &toksave);
        }
        free(buf);
    }
    for (int i = 0; kProtectedDefaults[i]; i++) WhitelistAdd(kProtectedDefaults[i]);
}

void WhitelistAdd(const wchar_t* token)
{
    if (g_whitelistN >= 128 || !token || !*token) return;
    for (int i = 0; i < g_whitelistN; i++)
        if (_wcsicmp(g_whitelist[i], token) == 0) return;
    wcsncpy(g_whitelist[g_whitelistN], token, 79);
    g_whitelist[g_whitelistN][79] = 0;
    g_whitelistN++;
    WhitelistPersist();
}

bool WhitelistRemoveAt(int idx)
{
    if (idx < 0 || idx >= g_whitelistN) return false;
    for (int i = 0; kProtectedDefaults[i]; i++)
        if (_wcsicmp(g_whitelist[idx], kProtectedDefaults[i]) == 0)
            return false;                    // 核心保护不可移除
    memmove(g_whitelist[idx], g_whitelist[idx + 1],
            (size_t)(g_whitelistN - idx - 1) * sizeof(g_whitelist[0]));
    g_whitelistN--;
    WhitelistPersist();
    return true;
}

int WhitelistCount(void) { return g_whitelistN; }
const wchar_t* WhitelistGet(int idx)
{
    return (idx >= 0 && idx < g_whitelistN) ? g_whitelist[idx] : L"";
}

bool WhitelistContains(const wchar_t* exeNameLower, const wchar_t* fullPath)
{
    for (int i = 0; i < g_whitelistN; i++) {
        const wchar_t* w = g_whitelist[i];
        if (wcschr(w, L'\\')) {
            if (fullPath && _wcsnicmp(fullPath, w, wcslen(w)) == 0) return true;
        } else if (_wcsicmp(exeNameLower, w) == 0) return true;
    }
    return false;
}

bool IsSystemCritical(const wchar_t* exeNameLower)
{
    static const wchar_t* k[] = {
        L"svchost.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
        L"services.exe", L"lsass.exe", L"smss.exe", L"dwm.exe",
        L"explorer.exe", L"taskhostw.exe", L"fontdrvhost.exe",
        L"memorycompressor.exe", L"wudfhost.exe", L"audiodg.exe",
        L"searchindexer.exe", L"sihost.exe", L"ctfmon.exe",
        L"startmenuexperiencehost.exe", L"textinputhost.exe", L"runtimebroker.exe",
        L"backgroundtaskhost.exe", L"securityhealthservice.exe",
        L"msmpeng.exe", L"nissrv.exe", L"widgets.exe", L"widgetserver.exe", NULL };
    for (int i = 0; k[i]; i++) if (_wcsicmp(k[i], exeNameLower) == 0) return true;
    return false;
}

bool IsGameProcess(DWORD pid, const wchar_t* exeName)
{
    (void)pid;
    if (_wcsicmp(exeName, L"cs2.exe") == 0) return true;
    return WhitelistContains(exeName, NULL);
}

// ------------------------------------------------------------------ 设置
Settings g_set = { 0, 1, 1, 1, 0 };   // Smart / 普通 / 剔除超线程 / 1ms 开

void SettingsLoad(void)
{
    wchar_t* buf = NULL; size_t len = 0;
    ReadFileText(g_pathSet, &buf, &len);
    if (!buf) return;
    wchar_t* toksave = NULL; wchar_t* tok = TokW(buf, L"\r\n", &toksave);
    while (tok) {
        wchar_t* eq = wcschr(tok, L'=');
        if (eq) {
            *eq = 0;
            int v = _wtoi(eq + 1);
            if      (!_wcsicmp(tok, L"mode"))      g_set.mode = v;
            else if (!_wcsicmp(tok, L"intensity")) g_set.intensity = v;
            else if (!_wcsicmp(tok, L"policy"))    g_set.policy = v;
            else if (!_wcsicmp(tok, L"timers"))    g_set.timers = v;
            else if (!_wcsicmp(tok, L"autoguard")) g_set.autoGuard = v;
        }
        tok = TokW(NULL, L"\r\n", &toksave);
    }
    free(buf);
}

void SettingsSave(void)
{
    wchar_t s[256];
    swprintf(s, 256, L"mode=%d\nintensity=%d\npolicy=%d\ntimers=%d\nautoguard=%d\n",
             g_set.mode, g_set.intensity, g_set.policy, g_set.timers, g_set.autoGuard);
    WriteFileAtomic(g_pathSet, s);
}

// ------------------------------------------------------------------ CPU 拓扑
CpuTopo g_topo;

bool TopoQuery(void)
{
    memset(&g_topo, 0, sizeof(g_topo));
    DWORD len = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0)
        return false;
    BYTE* buf = (BYTE*)malloc(len);
    if (!buf) return false;
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        free(buf); return false;
    }
    size_t hdr = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
    BYTE maxC = 0;
    size_t off = 0;
    // 第一遍找最大效率类
    while (off + hdr <= len) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX rec =
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
        if (rec->Size == 0 || off + rec->Size > len) break;
        if (rec->Relationship == RelationProcessorCore &&
            rec->Processor.EfficiencyClass > maxC)
            maxC = rec->Processor.EfficiencyClass;
        off += rec->Size;
    }
    // 第一遍 B: 判定哪一组是性能核
    //   Windows 的 EfficiencyClass 越大越"省电"，但个别平台方向相反；
    //   更可靠的判据是 SMT: 支持超线程的物理核一定是 P-core。
    //   两者取交集，冲突时以 SMT 为准。
    {
        int smtCores = 0, nonSmtCores = 0;
        BYTE smtEff = 0, nonSmtEff = 0;
        off = 0;
        while (off + hdr <= len) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX rec =
                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (rec->Size == 0 || off + rec->Size > len) break;
            if (rec->Relationship == RelationProcessorCore &&
                rec->Processor.GroupCount > 0) {
                KAFFINITY m = rec->Processor.GroupMask[0].Mask;
                // SMT = 该物理核掩码含 >=2 个逻辑核
                bool smt = (rec->Processor.Flags & LTP_PC_SMT) != 0
                           && m && (m & (m - 1)) != 0;
                if (smt) { smtCores++; smtEff = rec->Processor.EfficiencyClass; }
                else if (m) { nonSmtCores++; nonSmtEff = rec->Processor.EfficiencyClass; }
            }
            off += rec->Size;
        }
        // 只有一组带 SMT 且两组效率类不同 -> 该组的 effClass 才是 P-core 的值
        if (smtCores > 0 && nonSmtCores > 0 && smtEff != nonSmtEff)
            maxC = smtEff;
    }
    // 第二遍按逻辑核展开
    int idx = 0;
    off = 0;
    while (off + hdr <= len) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX rec =
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
        if (rec->Size == 0 || off + rec->Size > len) break;
        if (rec->Relationship == RelationProcessorCore &&
            rec->Processor.GroupCount > 0) {
            KAFFINITY m = rec->Processor.GroupMask[0].Mask;
            BYTE ec = rec->Processor.EfficiencyClass;
            for (int bit = 0; bit < 64 && idx < 256; bit++) {
                if (m & ((KAFFINITY)1 << bit)) {
                    g_topo.physId[idx] = idx;
                    g_topo.effClass[idx] = ec;
                    idx++;
                }
            }
            if (m) {
                g_topo.allMask |= (DWORD_PTR)m;
                g_topo.physOnePerCoreMask |= (DWORD_PTR)(m & (~(DWORD_PTR)m + 1));
                if (ec == maxC) g_topo.pCoreMask |= (DWORD_PTR)m;
                else            g_topo.eCoreMask |= (DWORD_PTR)m;
            }
        }
        off += rec->Size;
    }
    free(buf);
    g_topo.count = idx;
    g_topo.maxEff = maxC;
    g_topo.hybrid = (g_topo.eCoreMask != 0);
    g_topo.ok = (idx > 0);
    return g_topo.ok;
}

const wchar_t* TopoClassLabel(int logicalIdx)
{
    if (!g_topo.ok || logicalIdx < 0 || logicalIdx >= g_topo.count) return L"?";
    if (!g_topo.hybrid) return L"C";
    // 与 pCoreMask/eCoreMask 同一判据: effClass == maxEff 即 P-core
    return (g_topo.effClass[logicalIdx] == g_topo.maxEff) ? L"P" : L"E";
}
