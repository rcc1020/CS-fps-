// app.h — CS2 优化器 v2.0 共享声明
// 架构借鉴 Pavise-Game (dulaiduwang003)：会话事务 + 退出还原 + 恢复守卫 +
// 后台智能抑制 + 白名单保护 + 系统体检 + 标签式图形面板 + 双日志。
// 实现仍为纯 C / Win32，零依赖，不触碰游戏内存。
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <powrprof.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#define APP_NAME        L"CS2 优化器"
#define APP_VERSION     L"v2.0"
#define APP_TITLE       L"CS2 帧率与延迟优化面板 (Pure C / Win32)"
#define JOURNAL_MAGIC   L"CS2OPT-JOURNAL/2"

// ---------------------------------------------------------------- 目录与文件
void      AppDirsInit(void);
const wchar_t* AppDir(void);        // %APPDATA%\CS2Optimizer
const wchar_t* AppLogDir(void);     // ...\logs
const wchar_t* AppJournalPath(void);
const wchar_t* AppWhitelistPath(void);
const wchar_t* AppSettingsPath(void);

// ---------------------------------------------------------------- 双日志
// 操作日志 = 用户可读"本次做了什么"；诊断日志 = 技术细节。
// 同时写入 GUI 队列、action.log、diagnostic.log。
void  LogInit(void);
void  LogAction(const wchar_t* fmt, ...);          // 操作日志(进 GUI + session log)
void  LogDiag(const wchar_t* fmt, ...);            // 诊断日志
void  LogDiagWin(const wchar_t* what);             // 诊断 + GetLastError
void  GuiLogSetWnd(HWND hwnd);                     // 由 gui.c 提供
void  GuiLogDrain(HWND hAction, HWND hDiag);       // UI 线程消费两条队列 (可传 NULL 只清队)
void  LogFlushToFiles(void);                       // 定时落盘

// 日志内存环形缓冲直读 (供自绘日志控件/导出, 线程安全)
int   LogLineCount(int q);                         // q: 0=操作 1=诊断
bool  LogGetLine(int q, int idx, wchar_t* out, int cch);
void  LogClearMemory(int q);

// GUI 消息
#define WM_APP_LOGQ   (WM_APP + 5)   // 操作日志队列有内容
#define WM_APP_SESS   (WM_APP + 6)   // 会话状态变化 (守护线程 -> UI)

// ---------------------------------------------------------------- 小工具
bool  IsRunAsAdmin(void);
bool  RelaunchElevated(int autoClickId);
void  ReadFileText(const wchar_t* path, wchar_t** outBuf, size_t* outLen);
bool  WriteFileAtomic(const wchar_t* path, const wchar_t* text); // temp+replace
void  EnsureDirTree(const wchar_t* path);
bool  FileExistsW(const wchar_t* path);
void  U8ToW(const char* u8, wchar_t* out, int cch);
void  WToU8(const wchar_t* w, char* out, int cb);
bool  ParseGuidText(const wchar_t* s, GUID* out);
void  FormatGuidText(const GUID* g, wchar_t* out, size_t cch);

// ---------------------------------------------------------------- 会话事务
typedef enum {
    JK_NONE = 0,
    JK_REG_DWORD,       // 注册表 DWORD (持久)
    JK_REG_SZ,          // 注册表字符串 (持久)
    JK_POWERPLAN,       // 电源方案 (持久)
    JK_PRIORITY,        // 进程优先级类 (会话)
    JK_AFFINITY,        // 进程亲和性 (会话)
    JK_ECOQOS,          // 进程电源节流 (会话)
    JK_MEMPRIORITY,     // 进程内存优先级 (会话)
    JK_FSO_LAYER        // AppCompat Layer (持久)
} JournalKind;

typedef struct {
    JournalKind kind;
    bool        persistent;   // true=持久设置(手动还原), false=会话(游戏退出自动还原)
    bool        applied;
    wchar_t     key[260];     // 注册表 "HKCU|Sub|Value" / exe 路径 / "pid"
    wchar_t     name[80];     // 值名 / 描述
    wchar_t     oldStr[260];  // 旧字符串值; 首字节 0x01 表示原本不存在
    wchar_t     aux[80];      // 电源方案旧 GUID 字符串
    uint64_t    oldDword;     // 旧 DWORD / 旧掩码 / 旧优先级
    uint64_t    newDword;
    DWORD       pid;
    uint64_t    createTime;   // 进程创建时间，防 PID 复用误还原
} JournalEntry;

#define JOURNAL_MAX 512
extern JournalEntry g_journal[JOURNAL_MAX];
extern int          g_journalCount;
extern volatile LONG g_suppressedCount;   // 概览页计数
extern volatile LONG g_restoredCount;

void JournalLoadAtBoot(void);             // 启动时崩溃恢复
void JournalAdd(const JournalEntry* e);
bool JournalFindByKey(JournalKind kind, const wchar_t* key, const wchar_t* name, JournalEntry** out);
void JournalSaveToDisk(void);
void JournalRestore(bool persistentOnly, bool sessionOnly, const wchar_t* reason);
bool JournalRevertRegKey(const wchar_t* key);   // 按 "HKxx|Sub|Value" 还原单条并出队
void JournalClearDisk(void);

// ---------------------------------------------------------------- 白名单/保护
void  WhitelistInit(void);
bool  WhitelistContains(const wchar_t* exeNameLower, const wchar_t* fullPath);
void  WhitelistAdd(const wchar_t* token);
bool  WhitelistRemoveAt(int idx);
int   WhitelistCount(void);
const wchar_t* WhitelistGet(int idx);
bool  IsSystemCritical(const wchar_t* exeNameLower);   // 永不抑制
bool  IsGameProcess(DWORD pid, const wchar_t* exeName); // 受保护(游戏链)

// ---------------------------------------------------------------- 设置
typedef struct {
    int  mode;          // 0 Smart 1 竞技
    int  intensity;     // 0 关 1 普通 2 激进
    int  policy;        // 亲和性策略 (CPU 页)
    int  timers;        // 1ms 时钟
    int  autoGuard;     // 开机自动守护 (仅记录)
} Settings;
extern Settings g_set;
void SettingsLoad(void);
void SettingsSave(void);

// ---------------------------------------------------------------- CPU 拓扑
typedef struct {
    int      count;                 // 逻辑核数 (Group0)
    int      physId[256];           // 每逻辑核所属物理核序号
    BYTE     effClass[256];         // 效率类 (越大越省电 = E-core)
    BYTE     maxEff;                // 本机最大效率类 (= P-core 的值)
    DWORD_PTR allMask, physOnePerCoreMask, pCoreMask, eCoreMask;
    bool     hybrid;
    bool     ok;
} CpuTopo;
extern CpuTopo g_topo;
bool  TopoQuery(void);
const wchar_t* TopoClassLabel(int logicalIdx);   // "P" / "E" / "-"

// CPU 亲和性策略 (与 GUI 下拉项序一致)
typedef enum {
    AFF_KEEP_ALL = 0,
    AFF_PHYSICAL_ONLY,
    AFF_DROP_ECORES,
    AFF_ALL_CORES,
    AFF_CORE0
} AffPolicy;

// ---------------------------------------------------------------- 系统优化项
typedef struct {
    const wchar_t* id;
    const wchar_t* label;
    const wchar_t* desc;
    HKEY    hive;
    const wchar_t* sub;
    const wchar_t* val;
    DWORD   value;
    bool    needsAdmin;
    bool    needsReboot;
} RegTweak;
const RegTweak* TweakTable(int* count);
bool TweakIsApplied(const RegTweak* t);
bool TweakApply(const RegTweak* t);
bool TweakRevert(const RegTweak* t);
void TweaksApplyAll(void);
void TweaksRevertPersistent(void);

bool PowerApplyHighPerformance(void);
// 电源基线: 守护启动时采集, 避免被 ProcessLasso/Bitsum 等外部工具抢先改写后污染还原目标
void PowerCaptureBaseline(void);
void PowerForgetBaseline(void);
bool GetCs2PathForCheckup(wchar_t* out, size_t cch);
void FsoDisableForCs2(void);
void HagsToggle(void);
void CfgGenerateAutoexec(void);
void PrewarmCs2WorkingSet(void);   // CS2 工作集整理 (EmptyWorkingSet)

// ---------------------------------------------------------------- 进程与调优
DWORD FindCs2Process(wchar_t* outPath, size_t cchPath);
HANDLE OpenCs2(DWORD pid);
void TuneCs2Now(bool fromAuto);            // 优先级/亲和性/EcoQoS/内存优先级
void ApplyAffinityPolicy(DWORD pid, int policy);
void Cs2AffinityRestore(DWORD pid);

// ---------------------------------------------------------------- 后台抑制
void SuppressTick(bool gameActive);        // 守护线程周期调用
void SuppressRestoreSession(void);         // 游戏退出还原
int  SuppressActiveCount(void);
int  SuppressSnapshot(DWORD* pids, wchar_t names[][32], int max); // 供 GUI 列表

// ---------------------------------------------------------------- 体检
typedef struct {
    wchar_t text[200];
    int     level;      // 0 实锤-已优化 1 提示 2 实锤-需修复 3 未知
    int     fixTweakIdx;// 关联 RegTweak 表索引，-1 无
    int     fixKind;    // 0 无 1 tweak 2 powerplan 3 fso 4 hags 5 cfg
} Finding;
int  CheckupRun(Finding* out, int max);
void CheckupFixAll(const Finding* f, int n);

// ---------------------------------------------------------------- GUI
HWND  GuiCreate(HINSTANCE hi, int nCmdShow);
void  GuiInitGfx(void);
void  GuiOnTick(void);                 // 定时器: 曲线/状态刷新
void  GuiSetGuardState(bool on);
void  GuiNotifySession(bool active, DWORD pid);
void  GuiRefreshWhitelist(void);
void  GuiRefreshTweaks(void);
void  GuiRefreshSuppressList(void);
void  GuiRefreshCheckup(void);
void  GuiRefreshCpuGrid(void);
void  GuiShowPage(int idx);              // 切页 (含 --page= 直达)

// ---------------------------------------------------------------- 守护
extern volatile LONG g_guardActive;      // 用户开关
extern volatile LONG g_gameActive;       // 检测到游戏
extern volatile LONG g_gamePid;
void  GuardStart(void);
void  GuardStop(void);
void  GuardSetTimers(bool on);
