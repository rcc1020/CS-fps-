@echo off
chcp 936 >nul
echo ==============================================
echo   CS2 Win32 面板 v2.0 - 一键编译脚本
echo ==============================================
echo.

set "OUT=CS2_Optimizer_GUI.exe"
set "SRC=src\main.c src\core.c src\tweaks.c src\proc.c src\checkup.c src\gui.c"
set "RES=src\app.res"

rem ---------- 方式 A: MinGW-w64 GCC ----------
where gcc >nul 2>&1
if %errorlevel% neq 0 goto no_gcc

echo [+] 检测到 MinGW-w64 GCC，正在编译...
set "RESARG="
where windres >nul 2>&1
if %errorlevel% neq 0 goto skip_res
windres -i src\app.rc -o %RES% -O coff
if %errorlevel% equ 0 set "RESARG=%RES%"
:skip_res
if not defined RESARG echo [!] 未嵌入资源清单，功能不受影响

gcc %SRC% %RESARG% -o %OUT% -mwindows -municode -lwinmm -ladvapi32 -lgdi32 -luser32 -lshell32 -lcomctl32 -lpsapi -lpowrprof -lshlwapi -O2 -s -Wall
if not exist %OUT% goto gcc_fail
echo [+] 编译成功！已生成: %OUT%
echo [*] 直接双击运行即可；需要管理员的项会标注。
goto done

:gcc_fail
echo [-] 编译失败，请检查上方报错信息。
goto done

:no_gcc
rem ---------- 方式 B: MSVC ----------
where cl >nul 2>&1
if %errorlevel% neq 0 goto no_msvc

echo [+] 检测到 MSVC cl.exe，正在编译...
rc /nologo /fo %RES% src\app.rc
if %errorlevel% neq 0 echo [!] rc.exe 资源编译失败，跳过资源嵌入

cl /nologo /W3 /O2 /MT /utf-8 /DUNICODE /D_UNICODE %SRC% /Fe:%OUT% /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib winmm.lib advapi32.lib shell32.lib comctl32.lib psapi.lib powrprof.lib shlwapi.lib %RES%
if exist *.obj del /q *.obj
if not exist %OUT% goto msvc_fail
echo [+] 编译成功！已生成: %OUT%
goto done

:msvc_fail
echo [-] 编译失败，请检查上方报错信息。
goto done

:no_msvc
echo [-] 未检测到 GCC (MinGW-w64) 或 MSVC cl.exe。
echo [*] 请安装 MinGW-w64 并把 bin 目录加入 PATH，
echo     或在 "Developer Command Prompt for VS" 中运行本脚本。
echo [*] 也可以: cmake -B build 然后 cmake --build build --config Release

:done
echo.
pause
