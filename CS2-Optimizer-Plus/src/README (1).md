# CS2 帧率与延迟优化面板 (Pure C / Win32) v2.0

纯 C + 原生 Win32 API 编写的 Counter-Strike 2 帧率与系统延迟优化工具。
**不读游戏内存、不注入、不挂钩（hook）、不含任何游戏内改动** —— 全部操作只作用于操作系统调度与图形栈开关。

> 设计参考 [dulaiduwang003/Pavise-Game](https://github.com/dulaiduwang003/Pavise-Game) 的产品范式（会话事务 / 退出自动还原 / 崩溃恢复 / 智能后台抑制 / 证据分级体检），
> 但用 C 重写为零依赖单 exe（92 KB），不依赖 .NET。

![概览](docs/pg_tweaks.png)

## 七页面板

| 页 | 内容 |
|---|---|
| 概览 | 会话状态、已抑制数、已还原数、权限卡片；CPU 180 点实时折线；守护开关；Smart/竞技模式；治理强度 |
| 系统优化 | 10 项注册表优化的勾选列表（勾选=已启用，双击/勾选即双向同步）、应用勾选项、一键全部应用、还原持久优化、高性能电源、禁用 FSO、HAGS 开关、生成 autoexec.cfg |
| CPU 与游戏 | 逻辑核网格（P-core 蓝 / E-core 黄，按真实 EfficiencyClass 分组）、亲和性策略下拉、立即调优、锁定工作集 |
| 后台抑制 | 被抑制进程实时列表（PID/名称/原优先级）、白名单编辑（核心项不可删）、治理强度说明 |
| 体检 | 证据分级结论（绿=已优化 / 橙红=需修复 / 蓝=提示 / 灰=未知或权限不足）+ 一键修复全部 |
| 还原中心 | journal 条目列表（类型/持久/状态）、还原会话项 / 还原持久项 / 全部还原 / 清空记录 |
| 日志 | 自绘深色日志控件（操作日志 + 诊断日志），支持滚轮/滚动条/自动跟随，可导出为 txt |

## 核心机制

### 1. 会话事务 + 退出自动还原
任何持久化改动（注册表、电源方案、进程优先级/亲和性/EcoQoS/内存优先级、FSO Layer）在写入前都会把**旧值**记入 journal：

```
%APPDATA%\CS2Optimizer\journal.txt      (文本格式，11 字段，tab 分隔，实时落盘)
```

- `persistent=0` 为会话项：`cs2.exe` 退出或守护停止时自动还原
- `persistent=1` 为持久项：仅在还原中心手动还原
- 每次写入用 `WriteFileAtomic`（临时文件 + `ReplaceFileW`），断电/强杀不会留下半截记录

### 2. 崩溃恢复
启动时 `JournalLoadAtBoot` 读取 journal，对所有 `applied=1` 且未被正常还原的条目自动回滚，并在日志里标记 `—— 开始还原：崩溃恢复 ——`。
即使工具被任务管理器强杀，系统状态也不会残留在"优化过"的中间态。

### 3. 电源基线防污染
守护启动的**那一刻**采集当前电源方案作为基线（`PowerCaptureBaseline`），而不是等检测到 cs2.exe 才读。

> 原因：ProcessLasso / Bitsum / 主板厂商工具会在游戏进程启动瞬间抢先切到自己的方案。若那时才读"旧值"，记录到的已是对手的方案，退出后就会还原成对手的值 —— 实测会把方案还原成 `d6f2f22d`（Bitsum Highest Performance）而不是用户原本的平衡方案。

还原后还会**回读校验**：实际方案与期望不符时，日志明确报
`[!] 电源方案被外部程序改写：期望 X，实际 Y` 并提示存在外部电源管理软件。

### 4. 大小核拓扑判定
`GetLogicalProcessorInformationEx(RelationProcessorCore)` 读取每个物理核的 `EfficiencyClass` 与 `SMT` 标志：

- 本机实测：core 0–7 `effClass=1` + `LTP_PC_SMT`（每核 2 逻辑核 → 逻辑核 0..15），core 8–23 `effClass=0` 无 SMT（逻辑核 16..31）
- 单核实测算力：逻辑核 0/8 = 30–34ms，逻辑核 16/20/24/31 = 60–63ms → 证实高 `EfficiencyClass` 组就是 P-core
- 判定规则：`SMT 组 vs 非 SMT 组的 EfficiencyClass 若不同，SMT 组的大值即 P-core 类`

> v1.0 用 `effClass >= 0xC0` 判 P-core 完全错误（实际值只有 0 和 1），导致 32 个核全标成 E-core。

### 5. 智能后台抑制
只治理**有可见窗口的桌面应用**（`PidHasVisibleWindow` 枚举顶层窗口），服务与系统关键进程（`IsSystemCritical`）和白名单一律不碰。

| 档位 | 动作 |
|---|---|
| 普通 | 优先级 → `BELOW_NORMAL`，内存优先级 → `LOW` |
| 激进 | 优先级 → `IDLE`，内存优先级 → `LOW`，再叠加 EcoQoS 效率模式 |

- **Smart 模式**：游戏不在前台即释放治理
- **竞技模式**：会话期间持续治理
- 还原时按记录的旧值精确回滚（含 `hadEco` 标志判断是否撤销 EcoQoS）

### 6. 双日志 + 自绘控件
- 操作日志（session log）与诊断日志（diagnostic log）分文件落盘：`logs\session-YYYYMMDD.log` / `logs\diagnostic-YYYYMMDD.log`
- UI 里用自绘控件 `CS2Log`（`WM_PAINT` + `DrawTextW`）而不是 EDIT

> 只读 EDIT 在启用视觉样式时由主题自绘背景、**忽略** `WM_CTLCOLORSTATIC` 返回的画刷（`SetTextColor` 却生效）→ 浅色字压白底不可见。最小复现（单层/嵌套页面）都无法重现，最终放弃这条路。

## 目录结构

```
src/
  app.h        共享契约 (192 行): JournalKind/JournalEntry/Settings/CpuTopo/RegTweak/Finding
  core.c       854 行: 目录初始化、journal 序列化与恢复、双日志、GUID 文本解析
  tweaks.c     461 行: 10 项注册表优化、电源方案、FSO、HAGS、autoexec.cfg 生成
  proc.c       382 行: 进程发现、亲和性/优先级/EcoQoS 调优、后台抑制引擎
  checkup.c    171 行: 体检采样与分级、一键修复分派
  gui.c        979 行: 七页 Tab、自绘控件 (CS2Graph/CS2Cards/CS2Cores/CS2Log)、列表与状态栏
  main.c       149 行: 单实例、守护线程、高精度时钟、命令行参数
  app.rc / app.manifest   资源清单 (Common-Controls v6 / asInvoker / PerMonitorV2)
docs/          七页界面截图
build.bat      一键编译 (GBK+CRLF，自动探测 GCC / MSVC)
CMakeLists.txt CMake 工程
```

## 编译

### MinGW-w64 GCC
```bash
windres -i src\app.rc -o src\app.res -O coff
gcc src\main.c src\core.c src\tweaks.c src\proc.c src\checkup.c src\gui.c src\app.res ^
    -o CS2_Optimizer_GUI.exe -mwindows -municode ^
    -lwinmm -ladvapi32 -lgdi32 -luser32 -lshell32 -lcomctl32 ^
    -lpsapi -lpowrprof -lshlwapi -O2 -s -Wall
```

### 一键脚本
双击 `build.bat`（自动探测 GCC / MSVC）。

### CMake
```bash
cmake -B build
cmake --build build --config Release
```

### 已知编译坑
- `windres` 输出必须加 `-O coff`，否则链接报 `file format not recognized`
- MinGW 的 `wcstok` 是 3 参数签名，需要包装
- `build.bat` 必须存为 GBK + CRLF，否则 cmd 解析断裂
- 重新链接前先关掉已运行的实例，否则 ld 报 `Permission denied`

## 使用建议

1. 普通权限即可启动；需要管理员的按钮会在点击时弹一次 UAC 并自动续跑（`--autoclick=N`）。
2. 先跑一次「体检 → 一键修复全部」把系统基线拉正，再开守护。
3. 亲和性策略：AMD / 无超线程选「保持默认」，Intel 大小核选「剔除 E-core」，其余「剔除超线程」最稳。
4. autoexec.cfg 生成后，务必在 CS2 启动项加 `+exec autoexec.cfg` —— CS2 不会自动执行。
5. 若装了 ProcessLasso / Bitsum，本工具的电源方案可能与其争抢；日志里出现「电源方案被外部程序改写」即为此因，建议在对手软件里排除 cs2.exe。

## v2.0 相对 v1.1

| 维度 | v1.1 | v2.0 |
|---|---|---|
| 架构 | 单文件 1000 行 | 六模块 + 共享头 (~3200 行) |
| 界面 | 5 个按钮 + 日志框 | 七页 Tab 面板 + 4 种自绘控件 |
| 还原 | 无 | journal 事务 + 崩溃恢复 + 还原中心 |
| 后台治理 | 无 | 智能/竞技 + 三档强度 + 白名单 |
| 体检 | 无 | 10+ 项采样、四级证据分级、一键修复 |
| 日志 | 单 EDIT | 双通道 + 分文件落盘 + 导出 |
| 拓扑 | `~3` 硬编码 | 真实 SMT/EfficiencyClass 判定 |
| 日志控件 | — | 自绘 `CS2Log`（规避主题吞画刷） |
